
#include <async_redis_client/async_redis_client.h>
#include <libuv_timer/timer_manager.h>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <hiredis_util/hiredis_util.h>


DEFINE_string(redis_host, "127.0.0.1", "redis host");
DEFINE_int32(redis_port, 6379, "redis port");
DEFINE_string(redis_passwd, "", "redis passwd");
DEFINE_int32(work_thread_num, 1, "redis async client work thread num");
DEFINE_int32(conn_per_thread, 1, "connection per thread");
DEFINE_int32(timeout, 1500, "ms");

void OnRedisReply(redisReply *reply) noexcept {
    if (reply) {
        LOG(INFO) << "reply: " << *reply;
    } else {
        LOG(ERROR) << "reply: NULL";
    }
    return ;
}

TimerManager g_timer_manager;
AsyncRedisClient g_async_redis_cli;

unsigned int OnTimer(unsigned int status) noexcept {
    if (status != 0) {
        LOG(ERROR) << "OnTimer status: " << status;
        return 0; // 返回值会被忽略.
    }

    try {
        g_async_redis_cli.Execute(std::vector<std::string>{"GET", "hello"}, OnRedisReply);
        LOG(INFO) << "OnTimer, status: " << status;
    } catch (const std::exception &e) {
        LOG(ERROR) << "OnTimer, Execute Error; exception: " << e.what();
    }
    return 0;
}

int main(int argc, char **argv) {
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

    g_timer_manager.thread_num = FLAGS_work_thread_num;
    g_timer_manager.Start();

    LOG(INFO) << "Start DONE";

    g_timer_manager.StartTimer(FLAGS_timeout, FLAGS_timeout, OnTimer);

    LOG(INFO) << "Join Begin";
    g_timer_manager.Join();
    g_async_redis_cli.Join();
    return 0;
}

