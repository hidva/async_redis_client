#include <sstream>

#include <rrid/scope_exit.h>
#include <common/utils.h>
#include <exception/errno_exception.h>

#include <hiredis/hiredis.h>


#include "async_redis_client/async_redis_client.h"




void AsyncRedisClient::Start() {
    if (thread_num <= 0 || conn_per_thread <= 0 || host.empty()) {
        THROW(EINVAL, "INVALID ARGUMENTS;");
    }

    work_threads_.resize(thread_num);
    for (size_t idx = 0; idx < thread_num; ++idx) {
        try {
            work_threads_[idx].thread = std::thread{WorkThread, this, idx};
            work_threads_[idx].started = true;
        } catch (...) {}
    }

    while (true) {
        bool has_unknown_status_thread = false;

        for (WorkThread &work_thread : work_threads_) {
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

    return ;
}


void AsyncRedisClient::DoStopOrJoin(ClientStatus op) {
    ClientStatus expect_status = ClientStatus::kInitial;
    bool cas_result = status_.compare_exchange_strong(expect_status, op,
        std::memory_order_relaxed, std::memory_order_relaxed);
    if (!cas_result) {
        std::stringstream str_stream;
        str_stream << "DoStopOrJoin ERROR! op: " << op << "; client_status: " << expect_status;
        throw std::runtime_error(str_stream.str());
    }

    for (WorkThread &work_thread : work_threads_) {
        if (!work_thread.started)
            continue;

        work_thread.AsyncSend();
    }

    for (WorkThread &work_thread : work_threads_) {
        if (!work_thread.started)
            continue ;

        try {
            work_thread.thread.join(); // join() 理论上不会抛出异常的.
        } catch (...) {}
    }

    return ;
}

AsyncRedisClient::~AsyncRedisClient() noexcept {
    ClientStatus current_status = status_.load(std::memory_order_relaxed);
    if (current_status == ClientStatus::kInitial)
        throw std::runtime_error("~AsyncRedisClient ERROR! current_status: kInitial");

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

        work_thread.request_vec->emplace_back(request, callback);
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
    LoopbackTraverse(work_threads_.begin(), work_threads_.end(), work_threads_.begin() + sn, AddTo);

    if (!add_success) {
        throw std::runtime_error("EXECUTE ERROR");
    }

    return ;
}

namespace {

struct RedisConnectionContext {
    WorkThreadContext *thread_ctx = nullptr;
    size_t idx;
    redisAsyncContext *hiredis_async_ctx = nullptr;
};

struct WorkThreadContext {
    AsyncRedisClient *client = nullptr;
    size_t idx;
    std::vector<RedisConnectionContext> conn_ctxs;
    uv_loop_t uv_loop;
};

} // namespace


/* 根据 AsyncRedisClient::~AsyncRedisClient() 得知在 AsyncRedisClient 对象被销毁之前已经调用了 Stop()
 * 或者 Join() 因此在 WorkThreadMain() 运行期间, client 指向的内存始终有效.
 */
void AsyncRedisClient::WorkThreadMain(AsyncRedisClient *client, size_t idx) noexcept {
    WorkThreadContext thread_ctx;
    thread_ctx.client = client;
    thread_ctx.idx = idx;

    ON_SCOPE_EXIT(on_thread_exit_1) {
        // 线程清理处理程序.
    };

    thread_ctx.hiredis_async_ctxs.reserve(client->conn_per_thread);
    for (size_t conn_idx = 0; conn_idx < )

}

