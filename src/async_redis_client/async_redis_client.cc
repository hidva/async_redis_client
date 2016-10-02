#include <sstream>

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

    work_threads_.reset(new std::vector<WorkThread>(thread_num));
    for (size_t idx = 0; idx < thread_num; ++idx) {
        try {
            (*work_threads_)[idx].thread = std::thread(WorkThreadMain, this, idx);
            (*work_threads_)[idx].started = true;
        } catch (...) {}
    }

    while (true) {
        bool has_unknown_status_thread = false;

        for (WorkThread &work_thread : *work_threads_) {
            if (work_thread.started && work_thread.GetStatus() == WorkThreadStatus::kUnknown) {
                has_unknown_status_thread = true;
                break;
            }
        }

        if (has_unknown_status_thread) {
            std::this_thread::yield();
        } else {
            break;
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
        work_thread.mux.lock();
        ON_SCOPE_EXIT(unlock_mux) {
            work_thread.mux.unlock();
        };

        if (!work_thread.request_vec) {
            return false;
        }

        work_thread.request_vec->emplace_back(new RedisRequest(request, callback));
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

    auto sn = seq_num.fetch_add(1, std::memory_order_relaxed);
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

redisAsyncContext* GetHIRedisAsyncCtx(const char *host, int port, uv_loop_t *loop, redisDisconnectCallback *fn) noexcept {
    redisAsyncContext *ac = redisAsyncConnect(host, port);
    if (ac == nullptr || ac->err != 0 ||
        redisLibuvAttach(ac, loop) != REDIS_OK ||
        redisAsyncSetDisconnectCallback(ac, fn) != REDIS_OK) {
        if (ac != nullptr) {
            redisAsyncFree(ac);
            return nullptr;
        }
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

    conn_ctx->hiredis_async_ctx = GetHIRedisAsyncCtx(thread_ctx->client->host.c_str(),
                                                     thread_ctx->client->port,
                                                     &thread_ctx->uv_loop,
                                                     OnRedisDisconnect);
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


} // namespace


/* 根据 AsyncRedisClient::~AsyncRedisClient() 得知在 AsyncRedisClient 对象被销毁之前已经调用了 Stop()
 * 或者 Join() 因此在 WorkThreadMain() 运行期间, client 指向的内存始终有效.
 */
void AsyncRedisClient::WorkThreadMain(AsyncRedisClient *client, size_t idx) noexcept {
    WorkThreadContext thread_ctx;
    thread_ctx.client = client;
    thread_ctx.idx_in_client = idx;
    WorkThread *work_thread = &(*client->work_threads_)[idx];
    thread_ctx.work_thread = work_thread;

    ON_SCOPE_EXIT(on_thread_exit_1){
        // 正常情况下, 这里每一步都不应该抛出异常. 如果某个步骤抛出了异常, 那表明是(极)不正常的情况. 此时会 terminate().
        std::unique_ptr<std::vector<std::unique_ptr<RedisRequest>>> request_vec;

        work_thread->mux.lock();

        if (work_thread->async_handle) {
            throw std::runtime_error("work_thread->async_handle != nullptr");
        }

        request_vec.reset(work_thread->request_vec.release()); // noexcept

        work_thread->status = WorkThreadStatus::kExiting; // noexcept

        work_thread->mux.unlock();

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
        // 所有可能会抛出异常的初始化操作都放在这里进行.

        /* 这里不需要在获取 work_thread->mux 之后再进行操作. 因为当执行到此处的时候, request_vec 只会被当前线程
         * 访问到, 其他线程仍然阻塞在 AsyncRedisClient::Start() 中呢.
         */
        work_thread->request_vec.reset(new std::vector<std::unique_ptr<RedisRequest>>);

        thread_ctx.conn_ctxs.resize(client->conn_per_thread);

        // 整个 for 循环不可能抛出异常.
        for (size_t conn_idx = 0; conn_idx < client->conn_per_thread; ++conn_idx) {
            RedisConnectionContext *conn_ctx = &thread_ctx.conn_ctxs[conn_idx];

            conn_ctx->hiredis_async_ctx = GetHIRedisAsyncCtx(client->host.c_str(), client->port,
                                                             &thread_ctx.uv_loop, OnRedisDisconnect);
            if (conn_ctx->hiredis_async_ctx != nullptr) {
                conn_ctx->hiredis_async_ctx->data = conn_ctx;

                conn_ctx->idx_in_thread_ctx = conn_idx;
                conn_ctx->thread_ctx = &thread_ctx;
            }
        }
        ON_EXCEPTIN {
            for (RedisConnectionContext &conn_ctx : thread_ctx.conn_ctxs) {
                if (conn_ctx.hiredis_async_ctx) {
                    redisAsyncFree(conn_ctx.hiredis_async_ctx);
                    conn_ctx.hiredis_async_ctx = nullptr;
                }
            }
        };

    } catch (...) {
        init_success = false;
    }

    if (init_success) {
        work_thread->mux.lock();
        work_thread->async_handle = async_handle; // noexcept
        work_thread->status = WorkThreadStatus::kRunning; // noexcept
        work_thread->mux.unlock();
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
        request_vec.reset(work_thread->request_vec.release()); // noexcept
        if (tmp != nullptr) {
            work_thread->request_vec.reset(tmp); // noexcept
        }
        work_thread->mux.unlock();

        // request_vec 绝对不会为 nullptr. 但是怎么说呢, 为了以防万一还是进行了检测.
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
        request_vec.reset(work_thread->request_vec.release());
        work_thread->status = WorkThreadStatus::kExiting;
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
        request_vec.reset(work_thread->request_vec.release());
        work_thread->status = WorkThreadStatus::kExiting;
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
