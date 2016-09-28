#include <iostream>
#include <chrono>
#include <thread>

#include <signal.h>

#include <gflags/gflags.h>
#include <glog/logging.h>

#include <common/utils.h>
#include <common/inline_utils.h>

#include <async_redis_client/async_redis_client.h>

DEFINE_string(redis_host, "127.0.0.1", "redis host");
DEFINE_int32(redis_port, 6379, "redis port");
DEFINE_int32(work_thread_num, 4, "redis async client work thread num");
DEFINE_int32(conn_per_thread, 3, "connection per thread");


void OnSig(int) {
	return ;
}

int main(int argc, char **argv) {
    signal(SIGINT, OnSig);

    google::SetUsageMessage("AsyncRedisClient Test");
    google::SetVersionString("1.0.0");
    google::ParseCommandLineFlags(&argc, &argv, false);
    google::InitGoogleLogging(argv[0]);
    google::InstallFailureSignalHandler();

    struct timespec start_b = {0, 0};
    struct timespec start_e = {0, 0};
    struct timespec join_b = {0, 0};
    struct timespec join_e = {0, 0};
  
    LOG(INFO) << "Start...";

    pause();

    {
#if 1
    AsyncRedisClient async_redis_cli;
    async_redis_cli.conn_per_thread = FLAGS_conn_per_thread;
    async_redis_cli.thread_num = FLAGS_work_thread_num;
    async_redis_cli.host = FLAGS_redis_host;
    async_redis_cli.port = FLAGS_redis_port;

    clock_gettime(CLOCK_REALTIME, &start_b);
    async_redis_cli.Start();
    clock_gettime(CLOCK_REALTIME, &start_e);
#endif
    
    LOG(INFO) << "Started ...";
    pause();    

#if 1
    clock_gettime(CLOCK_REALTIME, &join_b);
    async_redis_cli.Join();
    clock_gettime(CLOCK_REALTIME, &join_e);
#endif
    }

    LOG(INFO) << "Start use: " << GetTimespecDiff(start_e, start_b) << " ns, "
              << "Join use: " << GetTimespecDiff(join_e, join_b) << " ns, "; 

    pause();
    return 0;
}

