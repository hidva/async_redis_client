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

DEFINE_string(redis_host, "127.0.0.1", "redis host");
DEFINE_int32(redis_port, 6379, "redis port");
DEFINE_int32(work_thread_num, 4, "redis async client work thread num");
DEFINE_int32(conn_per_thread, 3, "connection per thread");
DEFINE_int32(test_thread_num, 1, "test thread number");
DEFINE_int32(req_per_thread, 1, "每个 test thread 发送的 redis request 数量");

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

    LOG(INFO) << "ON REDIS REPLY, " << GetTimespecDiff(on_reply_timepoint, commit_timepoint) << ", "
              << is_success_reply << ","
              << thread_id;
}

AsyncRedisClient async_redis_cli;

void ThreadMain() noexcept {
    for (int i = 0; i < FLAGS_req_per_thread; ++i) {
        OnRedisReply reply_callback;
        clock_gettime(CLOCK_REALTIME, &reply_callback.commit_timepoint);

        try {
            async_redis_cli.Execute(g_redis_cmd,
                                    std::make_shared<AsyncRedisClient::req_callback_t>(reply_callback));
        } catch (const std::exception &e) {
            LOG(INFO) << "Execute ERROR; exception: " << e.what();
        }
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

    std::cout << "按 CTRL+C Start..." << std::endl;
    pause();

    async_redis_cli.conn_per_thread = FLAGS_conn_per_thread;
    async_redis_cli.thread_num = FLAGS_work_thread_num;
    async_redis_cli.host = FLAGS_redis_host;
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

    pause();
    return 0;
}

