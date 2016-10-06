#include <sstream>
#include <new>

#include <rrid/scope_exit.h>
#include <common/utils.h>
#include <exception/errno_exception.h>
#include <hiredis_util/hiredis_util.h>

#include <hiredis/hiredis.h>
#include <hiredis/async.h>
#include <hiredis/adapters/libuv.h>


#include "async_redis_client/async_redis_client.h"




void AsyncRedisClient::Start() {
    if (thread_num <= 0 || conn_per_thread <= 0 || host.empty()) {
        THROW(EINVAL, "INVALID ARGUMENTS;");
    }

    std::vector<std::promise<void>> promises(thread_num);
    std::vector<std::future<void>> futures(thread_num);
    for (size_t idx = 0; idx < thread_num; ++idx) {
        futures[idx] = promises[idx].get_future();
    }

    work_threads_.reset(new std::vector<WorkThread>(thread_num));
    for (size_t idx = 0; idx < thread_num; ++idx) {
        try {
            (*work_threads_)[idx].thread = std::thread(WorkThreadMain, this, idx, &promises[idx]);
            (*work_threads_)[idx].started = true;
        } catch (...) {}
    }

    for (size_t idx = 0; idx < thread_num; ++idx) {
        if ((*work_threads_)[idx].started) {
            futures[idx].get();
        }
    }

    SetStatus(ClientStatus::kStarted);
    return ;
}


void AsyncRedisClient::DoStopOrJoin(ClientStatus op) {
    ClientStatus expect_status = ClientStatus::kStarted;
    bool cas_result = status_.compare_exchange_strong(expect_status, op,
        std::memory_order_relaxed, std::memory_order_relaxed);
    if (!cas_result) {
        std::stringstream str_stream;
        str_stream << "DoStopOrJoin ERROR! op: " << op << "; client_status: " << expect_status;
        throw std::runtime_error(str_stream.str());
    }

    for (WorkThread &work_thread : *work_threads_) {
        if (!work_thread.started)
            continue;

        work_thread.AsyncSend();
    }

    JoinAllThread();

    return ;
}

AsyncRedisClient::~AsyncRedisClient() noexcept {
    ClientStatus current_status = GetStatus();
    if (current_status == ClientStatus::kStarted)
        throw std::runtime_error("~AsyncRedisClient ERROR! current_status: kStarted");

    /* 是的, 即使 current_status 不为 kInitial, 此时析构也不是安全的.
     * 但是本来就说了, ~AsyncRedisClient() 不是线程安全的.
     */
    return ;
}

void AsyncRedisClient::Execute(const std::shared_ptr<std::vector<std::string>> &request,
             const std::shared_ptr<req_callback_t> &callback) {
    auto DoAddTo = [&] (WorkThread &work_thread) -> bool {
        std::unique_ptr<RedisRequest> tmp_ptr(new RedisRequest(request, callback));

        work_thread.mux.lock();
        ON_SCOPE_EXIT(unlock_mux) {
            work_thread.mux.unlock();
        };

        if (!work_thread.request_vec) {
            return false;
        }

        work_thread.request_vec->emplace_back(std::move(tmp_ptr));

        work_thread.AsyncSendUnlock();
        return true;
    };

    bool add_success = false;

    auto AddTo = [&] (std::vector<WorkThread>::iterator iter) noexcept -> int {
        try {
            return (add_success = DoAddTo(*iter));
        } catch (...) {
            return 0;
        }
    };

    unsigned int sn = seq_num.fetch_add(1, std::memory_order_relaxed);
    sn %= thread_num;
    LoopbackTraverse(work_threads_->begin(), work_threads_->end(), work_threads_->begin() + sn, AddTo);

    if (!add_success) {
        throw std::runtime_error("EXECUTE ERROR");
    }

    return ;
}

namespace {

struct WorkThreadContext;

struct RedisConnectionContext {
    WorkThreadContext *thread_ctx = nullptr;
    size_t idx_in_thread_ctx;

    // 不变量 36: 若不为 nullptr, 则表明其指向着的 ctx 可用;
    redisAsyncContext *hiredis_async_ctx = nullptr;
};

struct WorkThreadContext {
    AsyncRedisClient *client = nullptr;
    size_t idx_in_client;
    AsyncRedisClient::WorkThread *work_thread = nullptr;

    bool no_new_request = false;

    // 序列号, 用来实现 Round-robin 算法.
    size_t seq_num{0};

    /* conn_ctx, uv_loop 由使用者来负责释放内存.
     */
    std::vector<RedisConnectionContext> conn_ctxs;
    uv_loop_t uv_loop;
};

void OnRedisDisconnect(const struct redisAsyncContext *hiredis_async_ctx, int /* status */) noexcept;

redisAsyncContext* GetHIRedisAsyncCtx(/* const */ RedisConnectionContext *conn_ctx) noexcept {
    WorkThreadContext *thread_ctx = conn_ctx->thread_ctx;
    AsyncRedisClient *client = thread_ctx->client;

    redisAsyncContext *ac = redisAsyncConnect(client->host.c_str(), client->port);
    if (!ac) {
        return nullptr;
    }

    // 注意对 ac 调用 redisAsyncFree();
    if (ac->err != 0) {
        redisAsyncFree(ac);
        return nullptr;
    }

    if (redisLibuvAttach(ac, &thread_ctx->uv_loop) != REDIS_OK) {
        redisAsyncFree(ac);
        return nullptr;
    }

    if (!client->passwd.empty()) {
        int hiredis_rc = redisAsyncCommand(ac, nullptr, nullptr, "AUTH %b",
                          client->passwd.data(),
                          static_cast<size_t>(client->passwd.size()));
        if (hiredis_rc != REDIS_OK) {
            redisAsyncFree(ac);
            return nullptr;
        }
    }

    ac->data = conn_ctx;
    if (redisAsyncSetDisconnectCallback(ac, OnRedisDisconnect) != REDIS_OK) {
        throw std::runtime_error("redisAsyncSetDisconnectCallback FAILED");
    }
    return ac;
}


void OnRedisDisconnect(const struct redisAsyncContext *hiredis_async_ctx, int /* status */) noexcept {
    RedisConnectionContext *conn_ctx = (RedisConnectionContext*)hiredis_async_ctx->data;
    WorkThreadContext *thread_ctx = conn_ctx->thread_ctx;

    if (thread_ctx->no_new_request) {
        conn_ctx->hiredis_async_ctx = nullptr;
        return ;
    }

    conn_ctx->hiredis_async_ctx = GetHIRedisAsyncCtx(conn_ctx);
    return ;
}

/// 参见实现
uv_async_t* GetAsyncHandle(uv_loop_t *loop, uv_async_cb async_cb) noexcept {
    uv_async_t *handle = static_cast<uv_async_t*>(malloc(sizeof(uv_async_t)));
    if (handle == nullptr)
        return nullptr;

    int uv_rc = uv_async_init(loop, handle, async_cb);
    if (uv_rc < 0) {
        free(handle);
        return nullptr;
    }

    return handle;
}

void OnAsyncHandleClose(uv_handle_t* handle) noexcept {
    free(handle);
    return ;
}

void CloseAsyncHandle(uv_async_t *handle) noexcept {
    uv_close((uv_handle_t*)handle, OnAsyncHandleClose);
    return ;
}

inline void SetValueOn(std::promise<void> *p) noexcept {
    p->set_value();
    return ;
}

} // namespace


/* 根据 AsyncRedisClient::~AsyncRedisClient() 得知在 AsyncRedisClient 对象被销毁之前已经调用了 Stop()
 * 或者 Join() 因此在 WorkThreadMain() 运行期间, client 指向的内存始终有效.
 *
 * 注意 p 的生命周期.
 */
void AsyncRedisClient::WorkThreadMain(AsyncRedisClient *client, size_t idx, std::promise<void> *p) noexcept {
    WorkThreadContext thread_ctx;
    thread_ctx.client = client;
    thread_ctx.idx_in_client = idx;
    WorkThread *work_thread = &(*client->work_threads_)[idx];
    thread_ctx.work_thread = work_thread;

    ON_SCOPE_EXIT(on_thread_exit_1){
        // 正常情况下, 这里每一步都不应该抛出异常. 如果某个步骤抛出了异常, 那表明是(极)不正常的情况. 此时会 terminate().
        std::unique_ptr<std::vector<std::unique_ptr<RedisRequest>>> request_vec;
        {
            work_thread->mux.lock();
            request_vec.reset(work_thread->request_vec.release()); // noexcept
            work_thread->async_handle = nullptr;
            work_thread->mux.unlock();
        }

        if (p) {
            // 不变量 123: 若 p != nullptr, 则表明尚未对 p 调用过 set_xxx() 系列.
            SetValueOn(p);
            p = nullptr;
        }

        if (request_vec) {
            for (std::unique_ptr<RedisRequest> &redis_request : *request_vec) {
                redis_request->Fail();
            }
        }
    };

    if (uv_loop_init(&thread_ctx.uv_loop) < 0) {
        return ;
    }
    thread_ctx.uv_loop.data = &thread_ctx;
    ON_SCOPE_EXIT(on_thread_exit_2){
        int uv_rc = uv_loop_close(&thread_ctx.uv_loop);
        if (uv_rc < 0) {
            THROW(uv_rc, "uv_loop_close ERROR");
        }
    };

    // async_handle 指向的内存是手动管理的. 注意了.
    uv_async_t *async_handle = GetAsyncHandle(&thread_ctx.uv_loop, AsyncRedisClient::OnAsyncHandle);
    if (async_handle == nullptr) {
        return ;
    }
    async_handle->data = &thread_ctx;

    bool init_success = true;
    try {
        // 所有可能会抛出异常的初始化操作都放在这里进行. 只要确保这其中分配的资源正确释放就行了.

        std::unique_ptr<std::vector<std::unique_ptr<RedisRequest>>> tmp(new std::vector<std::unique_ptr<RedisRequest>>);
        work_thread->mux.lock();
        work_thread->request_vec = std::move(tmp);
        work_thread->async_handle = async_handle; // noexcept
        work_thread->mux.unlock();
        // 以上分配的资源会由 on_thread_exit_1 进行回收.

        thread_ctx.conn_ctxs.resize(client->conn_per_thread);

        // 整个 for 循环不可能抛出异常.
        for (size_t conn_idx = 0; conn_idx < client->conn_per_thread; ++conn_idx) {
            RedisConnectionContext *conn_ctx = &thread_ctx.conn_ctxs[conn_idx];

            conn_ctx->idx_in_thread_ctx = conn_idx;
            conn_ctx->thread_ctx = &thread_ctx;
            conn_ctx->hiredis_async_ctx = GetHIRedisAsyncCtx(conn_ctx);
        }
#if 0
        ON_EXCEPTIN {
            for (RedisConnectionContext &conn_ctx : thread_ctx.conn_ctxs) {
                if (conn_ctx.hiredis_async_ctx) {
                    redisAsyncFree(conn_ctx.hiredis_async_ctx);
                    conn_ctx.hiredis_async_ctx = nullptr;
                }
            }
        };
#endif
    } catch (...) {
        init_success = false;
    }

    if (init_success) {
        SetValueOn(p);
        p = nullptr;
    } else {
        CloseAsyncHandle(async_handle);
    }

    while (uv_run(&thread_ctx.uv_loop, UV_RUN_DEFAULT)) {
        ;
    }

    return ;
}

void AsyncRedisClient::OnRedisReply(redisAsyncContext * /* ac */, void *reply, void *privdata) noexcept {
    std::unique_ptr<RedisRequest> redis_request((RedisRequest*)privdata);
    redis_request->Success((redisReply*)reply);
    return ;
}

void AsyncRedisClient::OnAsyncHandle(uv_async_t* handle) noexcept {

    auto HandleRequest = [] (WorkThreadContext *thread_ctx, std::unique_ptr<RedisRequest> &request) noexcept {
        auto DoHandleRequestOn = [] (RedisConnectionContext &conn_ctx, std::unique_ptr<RedisRequest> &request) -> bool {
            if (!conn_ctx.hiredis_async_ctx) {
                return false;
            }

            int hiredis_rc = RedisAsyncCommandArgv(conn_ctx.hiredis_async_ctx, OnRedisReply,
                                                   request.get(), *request->cmd);
            if (hiredis_rc != REDIS_OK) {
                redisAsyncFree(conn_ctx.hiredis_async_ctx);
                return false;
            }
            request.release(); // 此后 RedisRequest 对象由 OnRedisReply 来负责管理.
            return true;
        };

        bool handle_success = false;

        auto HandleRequestOn = [&] (std::vector<RedisConnectionContext>::iterator iter) noexcept -> int {
            try {
                return handle_success = DoHandleRequestOn(*iter, request);
            } catch (...) {
                return 0;
            }
        };

        size_t begin_idx = (++thread_ctx->seq_num) % thread_ctx->conn_ctxs.size();
        LoopbackTraverse(thread_ctx->conn_ctxs.begin(), thread_ctx->conn_ctxs.end(),
                         thread_ctx->conn_ctxs.begin() + begin_idx,
                         HandleRequestOn);

        if (!handle_success) {
            request->Fail();
        }

        return ;
    };

    auto HandleRequests = [&] (WorkThreadContext *thread_ctx, std::vector<std::unique_ptr<RedisRequest>> &requests) noexcept {
        for (std::unique_ptr<RedisRequest> &request : requests) {
            HandleRequest(thread_ctx, request);
        }
        return ;
    };

    auto OnRequest = [&] (uv_async_t *handle) noexcept {
        WorkThreadContext *thread_ctx = (WorkThreadContext*)handle->data;
        WorkThread *work_thread = thread_ctx->work_thread;
        std::unique_ptr<std::vector<std::unique_ptr<RedisRequest>>> request_vec;

        auto *tmp = new(std::nothrow) std::vector<std::unique_ptr<RedisRequest>>;

        work_thread->mux.lock();
        request_vec = std::move(work_thread->request_vec); // noexcept
        work_thread->request_vec.reset(tmp); // noexcept
        work_thread->mux.unlock();

        if (request_vec) {
            HandleRequests(thread_ctx, *request_vec);
        }

        return ;
    };

    auto OnJoin = [&] (uv_async_t *handle) noexcept {
        WorkThreadContext *thread_ctx = (WorkThreadContext*)handle->data;
        WorkThread *work_thread = thread_ctx->work_thread;
        std::unique_ptr<std::vector<std::unique_ptr<RedisRequest>>> request_vec;

        work_thread->mux.lock();
        request_vec = std::move(work_thread->request_vec);
        work_thread->async_handle = nullptr;
        work_thread->mux.unlock();


        if (request_vec) {
            HandleRequests(thread_ctx, *request_vec);
        }

        thread_ctx->no_new_request = true;
        for (RedisConnectionContext &conn_ctx : thread_ctx->conn_ctxs) {
            if (!conn_ctx.hiredis_async_ctx)
                continue;
            redisAsyncDisconnect(conn_ctx.hiredis_async_ctx);
        }

        CloseAsyncHandle(handle);
        return ;
    };

    auto OnStop = [] (uv_async_t *handle) noexcept {
        WorkThreadContext *thread_ctx = (WorkThreadContext*)handle->data;
        WorkThread *work_thread = thread_ctx->work_thread;
        std::unique_ptr<std::vector<std::unique_ptr<RedisRequest>>> request_vec;

        work_thread->mux.lock();
        request_vec = std::move(work_thread->request_vec);
        work_thread->async_handle = nullptr;
        work_thread->mux.unlock();

        if (request_vec) {
            for (auto &request : *request_vec) {
                request->Fail();
            }
        }

        thread_ctx->no_new_request = true;
        for (RedisConnectionContext &conn_ctx : thread_ctx->conn_ctxs) {
            if (!conn_ctx.hiredis_async_ctx)
                continue;
            redisAsyncFree(conn_ctx.hiredis_async_ctx);
        }

        CloseAsyncHandle(handle);
        return ;
    };

    WorkThreadContext *thread_ctx = (WorkThreadContext*)handle->data;
    switch (thread_ctx->client->GetStatus(/* std::memory_order_relaxed */)) {
    case ClientStatus::kStarted:
        OnRequest(handle);
        break;
    case ClientStatus::kStop:
        OnStop(handle);
        break;
    case ClientStatus::kJoin:
        OnJoin(handle);
        break;
    default: // unreachable
        throw std::runtime_error("富强, 民主, 文明, 和谐, 自由, 平等, 公正, 法治, 爱国, 敬业, 诚信, 友善");
    }

    return ;
}

namespace {

struct PromiseCallback {
    std::shared_ptr<std::promise<AsyncRedisClient::redisReply_unique_ptr_t>> promise_end;

public:
    void operator()(redisReply *reply) noexcept;
};

/**
 * 将 right 移动到 left.
 *
 * @note 基于 hiredis commit:360a0646bb0f7373caab08382772ca0384c1fe6d 编写. 当 hiredis 版本迭代时, 注意
 * 调整.
 */
inline void MoveRedisReply(redisReply *left, redisReply *right) noexcept {
    *left = *right;
    right->type = REDIS_REPLY_NIL;
    return ;
}

/**
 * 移动 right.
 *
 * @return nullptr, 表明移动失败, 此时 right 不会有任何改动.
 *  非 nullptr, 表明移动成功, 此后 right 不可再被使用, 仍然可以安全地传给 freeRedisReply() 进行释放.
 */
inline redisReply* MoveRedisReply(redisReply *right) noexcept {
    redisReply *left = (redisReply*)malloc(sizeof(redisReply));
    if (!left) {
        return nullptr;
    }
    MoveRedisReply(left, right);
    return left;
}

void PromiseCallback::operator()(redisReply *reply) noexcept {
    if (!reply) {
        promise_end->set_exception(std::make_exception_ptr(std::runtime_error("reply: nullptr")));
        return ;
    }

    redisReply *reply_p = MoveRedisReply(reply);
    if (!reply_p) {
        promise_end->set_exception(std::make_exception_ptr(std::bad_alloc()));
    } else {
        promise_end->set_value(AsyncRedisClient::redisReply_unique_ptr_t(reply_p));
    }
    return ;
}


} // namespace

std::future<AsyncRedisClient::redisReply_unique_ptr_t>
AsyncRedisClient::Execute(const std::shared_ptr<std::vector<std::string>> &request) {
    PromiseCallback cb;
    cb.promise_end = std::make_shared<std::promise<AsyncRedisClient::redisReply_unique_ptr_t>>();
    auto future_end = cb.promise_end->get_future();
    Execute(request, std::make_shared<req_callback_t>(std::move(cb)));
    return std::move(future_end);
}

