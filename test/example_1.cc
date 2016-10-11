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
DEFINE_string(redis_passwd, "", "redis passwd");
DEFINE_int32(work_thread_num, 4, "redis async client work thread num");
DEFINE_int32(conn_per_thread, 3, "connection per thread");

void OnRedisReply(redisReply *reply) noexcept {
    LOG(INFO) << "reply: " << *reply;
    return ;
}


AsyncRedisClient g_async_redis_cli;

int main(int argc, char **argv) noexcept {
    google::SetUsageMessage("AsyncRedisClient Example 1");
    google::SetVersionString("1.0.0");
    google::ParseCommandLineFlags(&argc, &argv, false);
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();

    g_async_redis_cli.conn_per_thread = FLAGS_conn_per_thread;
    g_async_redis_cli.thread_num = FLAGS_work_thread_num;
    g_async_redis_cli.host = FLAGS_redis_host;
    g_async_redis_cli.passwd = FLAGS_redis_passwd;
    g_async_redis_cli.port = FLAGS_redis_port;
    g_async_redis_cli.Start();

    LOG(INFO) << "Started ...";

    // 同步方式.
    auto future_end = g_async_redis_cli.Execute(std::vector<std::string>{"SET", "hello", "world"});
    // future_end.wait_for(timeout); // 超时等待.
    auto reply = future_end.get();
    OnRedisReply(reply.get());

    // 异步方式.
    g_async_redis_cli.Execute(std::vector<std::string>{"GET", "hello"}, OnRedisReply);

    g_async_redis_cli.Join();

    return 0;
}

