#include <iostream>
#include <chrono>
#include <thread>

#include <signal.h>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <common/utils.h>
#include <common/inline_utils.h>
#include <hiredis_util/hiredis_util.h>

#include <async_redis_client/async_redis_client.h>

enum class ApiKind : int{
    kAsyncAsync = 0,
    kAsyncSync,
    kSync
};

DEFINE_string(redis_host, "127.0.0.1", "redis host");
DEFINE_int32(redis_port, 6379, "redis port");
DEFINE_string(redis_passwd, "", "redis passwd");
DEFINE_int32(work_thread_num, 4, "redis async client work thread num");
DEFINE_int32(conn_per_thread, 3, "connection per thread");
DEFINE_int32(test_thread_num, 1, "test thread number");
DEFINE_int32(req_per_thread, 1, "每个 test thread 发送的 redis request 数量");
DEFINE_bool(pause, false, "若为真, 则会调用 pause() 在某些时候");
DEFINE_int32(api_kind, (int)ApiKind::kAsyncSync, "测试所使用 api 的类型;0, kAsyncAsync; 1, kAsyncSync; 2, kSync");

void OnSig(int) {
    return ;
}

std::shared_ptr<std::vector<std::string>> g_redis_cmd = std::make_shared<std::vector<std::string>>();

inline bool IsSuccessReply(const struct redisReply *reply) noexcept {
    return  (reply &&
            (reply->type == REDIS_REPLY_STATUS) &&
            (MemCompare("OK", reply->str, reply->len) == 0));
}

struct OnRedisReply {
    struct timespec commit_timepoint = {0, 0};
    struct timespec on_reply_timepoint = {0, 0};

    void operator()(struct redisReply *reply) noexcept;
};

void OnRedisReply::operator()(struct redisReply *reply) noexcept {
    clock_gettime(CLOCK_REALTIME, &on_reply_timepoint);

    bool is_success_reply = IsSuccessReply(reply);
    auto thread_id = std::this_thread::get_id();
    const char *g_api_kind_desc[] {
        "kAsyncAsync",
        "kAsyncSync",
        "kSync" 
    };

    LOG(INFO) << "ON REDIS REPLY, " << g_api_kind_desc[FLAGS_api_kind] << ", " << GetTimespecDiff(on_reply_timepoint, commit_timepoint) << ", "
              << is_success_reply << ","
              << thread_id;
}

AsyncRedisClient async_redis_cli;

void AsyncAsyncThreadMain() noexcept {
    for (int i = 0; i < FLAGS_req_per_thread; ++i) {
        OnRedisReply reply_callback;
        clock_gettime(CLOCK_REALTIME, &reply_callback.commit_timepoint);

        try {
            async_redis_cli.Execute(*g_redis_cmd,
                                    std::move(reply_callback));
        } catch (const std::exception &e) {
            LOG(ERROR) << "Execute ERROR; exception: " << e.what();
        }
    }
    return ;
}

void AsyncSyncThreadMain() noexcept {
    for (int i = 0; i < FLAGS_req_per_thread; ++i) {
        try {
            OnRedisReply reply_callback;
            clock_gettime(CLOCK_REALTIME, &reply_callback.commit_timepoint);
            reply_callback(async_redis_cli.Execute(*g_redis_cmd).get().get());
        } catch (const std::exception &e) {
            LOG(ERROR) << "Execute ERROR; exception: " << e.what();
        }
    }
    return ;
}

namespace {
thread_local std::unique_ptr<redisContext,void(*)(redisContext *)> redis_ctx{nullptr, redisFree};

void OpenRedisContext() {
    if (redis_ctx)
        return ;

    redis_ctx = RedisConnect(FLAGS_redis_host.c_str(), FLAGS_redis_port);
    if (!redis_ctx || redis_ctx->err != 0) {
        redis_ctx.reset();
        THROW(EINVAL, "无法向 redis 建立连接; err: %s",
              redis_ctx ? redis_ctx->errstr : "UNKNOWN");
    }

    if (!FLAGS_redis_passwd.empty())
        RedisCommand(redis_ctx.get(), "AUTH %s", FLAGS_redis_passwd.c_str());

    return ;
}

void CloseRedisContext() noexcept {
    redis_ctx.reset();
    return ;
}

}

void SyncThreadMain() noexcept {
    for (int i = 0; i < FLAGS_req_per_thread; ++i) {
        try {
            OpenRedisContext();
            try {
                OnRedisReply reply_callback;
                clock_gettime(CLOCK_REALTIME, &reply_callback.commit_timepoint);
                auto reply = RedisCommandArgv(redis_ctx.get(), *g_redis_cmd);
                reply_callback(reply.get());
            } catch (...) {
                CloseRedisContext();
                throw ;
            }
        } catch (const std::exception &e) {
            LOG(ERROR) << "Execute ERROR; exception: " << e.what();
        }
    }
    return ;
}

void ThreadMain() noexcept {
    switch (FLAGS_api_kind) {
    case (int)ApiKind::kAsyncAsync:
        AsyncAsyncThreadMain();
        break;

    case (int)ApiKind::kAsyncSync:
        AsyncSyncThreadMain();
        break;

    case (int)ApiKind::kSync:
        SyncThreadMain();
        break;

    default: // unreachable
        throw std::runtime_error("WTF");
    }
    return ;
}

int main(int argc, char **argv) noexcept {
    signal(SIGINT, OnSig);

    g_redis_cmd->assign({"SET", "hello", "world"});
    google::SetUsageMessage("AsyncRedisClient Test");
    google::SetVersionString("1.0.0");
    google::ParseCommandLineFlags(&argc, &argv, false);
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();

    struct timespec start_b = {0, 0};
    struct timespec start_e = {0, 0};
    struct timespec join_b = {0, 0};
    struct timespec join_e = {0, 0};

    if (FLAGS_pause) {
        std::cout << "按 CTRL+C Start..." << std::endl;
        pause();
    }

    async_redis_cli.conn_per_thread = FLAGS_conn_per_thread;
    async_redis_cli.thread_num = FLAGS_work_thread_num;
    async_redis_cli.host = FLAGS_redis_host;
    async_redis_cli.passwd = FLAGS_redis_passwd;
    async_redis_cli.port = FLAGS_redis_port;

    clock_gettime(CLOCK_REALTIME, &start_b);
    async_redis_cli.Start();
    clock_gettime(CLOCK_REALTIME, &start_e);

    LOG(INFO) << "Started ...";

    std::vector<std::thread> test_threads;
    test_threads.reserve(FLAGS_test_thread_num);
    for (int i = 0; i < FLAGS_test_thread_num; ++i) {
        try {
            test_threads.emplace_back(ThreadMain);
        } catch (const std::exception &e) {
            LOG(ERROR) << "Start TEST Thread ERROR; exp: " << e.what();
        }
    }
    LOG(INFO) << "Test Thread Started; test_thread_num: " << test_threads.size();

    for (std::thread &test_thread : test_threads) {
        test_thread.join();
    }
    LOG(INFO) << "Test Thread Joined";

    clock_gettime(CLOCK_REALTIME, &join_b);
    async_redis_cli.Join();
    clock_gettime(CLOCK_REALTIME, &join_e);

    std::cout << "Start use: " << GetTimespecDiff(start_e, start_b) << " ns, "
              << "Join use: " << GetTimespecDiff(join_e, join_b) << " ns, " << std::endl;

    if (FLAGS_pause) {
        pause();
    }
    return 0;
}

