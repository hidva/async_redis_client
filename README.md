## Versioning

This project follows the [semantic versioning](http://semver.org/) scheme. The API change and backwards compatibility rules are those indicated by SemVer.


## 是什么

AsyncRedisClient 异步 Redis 客户端. AsyncRedisClient 会启动 `thread_num` 个线程, 每个线程具有 `conn_per_thread` 个到指定 redis 实例(由 `host:port` 来指定)的连接. 当通过 `AsyncRedisClient::Execute()` 来执行请求时, AsyncRedisClient 会(通过 round-robin 算法)选择一个线程, 然后将请求交给该线程来进行处理, 线程内部会(通过 round-robin 算法)选择一个连接来处理该请求, 并且得到响应之后调用指定的回调函数.

从上看来, AsyncRedisClient 不支持事务这类与连接相关的命令, 虽然可以提供一个重载形式的 Execute(), 如下:

```cpp
Execute(AsyncRedisClient::Connection conn, request, callback);
```

表示着在指定的连接上执行 request. 但是通过 redis.io 得知, 事务完全可以用 lua 脚本然后在一个请求中实现. 因此就没有提供这种形式的 Execute(). (毕竟一般形式的 Execute() 能不能被很好的实现都是一会事呢 @_@).

## 怎么用

1.  创建, 并启动一个 AsyncRedisClient 实例, 一般情况下, 一个进程内只需要一个 AsyncRedisClient 实例即可. 如下:

    ```cpp
    AsyncRedisClient g_async_redis_client;

    int main(int argc, char **argv) {
        g_async_redis_client.conn_per_thread = FLAGS_conn_per_thread;
        g_async_redis_client.thread_num = FLAGS_work_thread_num;
        g_async_redis_client.host = FLAGS_redis_host;
        g_async_redis_client.port = FLAGS_redis_port;

        g_async_redis_client.Start();
        // 之后就可以调用 g_async_redis_client.Execute() 来提交请求了.
    }
    ```

2.  通过 `AsyncRedisClient::Execute()` 系列 API 来提交请求, API 语义可以参考注释. 这里提供个栗子:

    ```cpp
    void OnRedisReply(struct redisReply *reply) noexcept {
        std::cout << *reply << std::endl;
    }

    // 待发送的请求命令.
    std::shared_ptr<std::vector<std::string>> g_redis_cmd = std::make_shared<std::vector<std::string>>();
    g_redis_cmd->assign({"SET", "hello", "world"});

    // 异步方式.
    g_async_redis_client.Execute(g_redis_cmd,
        std::make_shared<AsyncRedisClient::req_callback_t>(OnRedisReply));

    // 同步方式.
    auto future_end = g_async_redis_client.Execute(g_redis_cmd);
    auto reply = future_end.get();
    OnRedisReply(reply.get());
    ```

3.  停止 AsyncRedisClient 实例, AsyncRedisClient 提供了 `AsyncRedisClient::Join()`, `AsyncRedisClient::Stop()` 用来停止实例, 区别可以参考注释.

### 安装

1.  安装 libuv, 参见 libuv 手册.
2.  安装 hiredis, 建议使用 https://github.com/pp-qq/hiredis 这个. 与原 hiredis 相比, bugfix 更勤快一点; 至于编译安装方式与原 hiredis 一致.
3.  Ok

## DEMO1

效果展示, 注: 下面的 `./bin/test` 可以通过 test 目录下的 `make` 编译得到.

```shell
# 启动 2 个工作线程, 每个工作线程维持 10 个连接.
# 启动 4 个测试线程, 每个线程发送 10000 个请求.
$ time ./bin/test -conn_per_thread=10 -work_thread_num=2 -test_thread_num=4 -req_per_thread=10000
按 CTRL+C Start...
^CStart use: 4085753 ns, Join use: 373390141 ns,
^C
real	0m1.664s
user	0m0.376s
sys	0m0.036s
$ grep 'ON REDIS REPLY' /tmp/test.INFO  | wc -l
40000
# 40000 个请求在 1s 内处理完毕

# 查看处理失败的请求. 呐, 木有
$ grep 'ON REDIS REPLY' /tmp/test.INFO  | grep -v ', 1,'

# 这里的输出格式参考 test/main.cc.
$ grep 'ON REDIS REPLY' /tmp/test.INFO  | head -n 10
I0929 12:51:29.669737 25704 main.cc:48] ON REDIS REPLY, 156281, 1,139663492212480
I0929 12:51:29.669839 25705 main.cc:48] ON REDIS REPLY, 242292, 1,139663483819776
I0929 12:51:29.669944 25704 main.cc:48] ON REDIS REPLY, 80462, 1,139663492212480
I0929 12:51:29.670032 25705 main.cc:48] ON REDIS REPLY, 163700, 1,139663483819776
I0929 12:51:29.670132 25704 main.cc:48] ON REDIS REPLY, 78334, 1,139663492212480
I0929 12:51:29.670222 25705 main.cc:48] ON REDIS REPLY, 162460, 1,139663483819776
I0929 12:51:29.670321 25704 main.cc:48] ON REDIS REPLY, 77634, 1,139663492212480
I0929 12:51:29.670408 25705 main.cc:48] ON REDIS REPLY, 159135, 1,139663483819776
I0929 12:51:29.670506 25704 main.cc:48] ON REDIS REPLY, 74546, 1,139663492212480
I0929 12:51:29.670593 25705 main.cc:48] ON REDIS REPLY, 157011, 1,139663483819776
```

## DEMO2

基准测试, 比较几种 redis 请求的耗时. 代码参考 test/main.cc

```shell
wangwei|iZ25lfbcwlvZ|~/tmp/async_redis_client
$ bash -x new_test.sh -api_kind=0 -conn_per_thread=1 -work_thread_num=1
+ ./new_test -conn_per_thread=10 -work_thread_num=2 -test_thread_num=10 -req_per_thread=10000 -api_kind=0 -conn_per_thread=1 -work_thread_num=1
Start use: 332307 ns, Join use: 1018266672 ns,

real	0m1.199s
user	0m0.952s
sys	0m0.056s
++ wc -l
++ grep 'ON REDIS REPLY,' /tmp/new_test.INFO
+ echo 总请求条数: 100000
总请求条数: 100000
++ wc -l
++ grep -v ', 1,'
++ grep 'ON REDIS REPLY,' /tmp/new_test.INFO
+ echo 失败请求条数: 0
失败请求条数: 0
wangwei|iZ25lfbcwlvZ|~/tmp/async_redis_client
$ bash -x new_test.sh -api_kind=1 -conn_per_thread=1 -work_thread_num=1
+ ./new_test -conn_per_thread=10 -work_thread_num=2 -test_thread_num=10 -req_per_thread=10000 -api_kind=1 -conn_per_thread=1 -work_thread_num=1
Start use: 370193 ns, Join use: 144057 ns,

real	0m3.940s
user	0m2.432s
sys	0m1.104s
++ wc -l
++ grep 'ON REDIS REPLY,' /tmp/new_test.INFO
+ echo 总请求条数: 100000
总请求条数: 100000
++ wc -l
++ grep -v ', 1,'
++ grep 'ON REDIS REPLY,' /tmp/new_test.INFO
+ echo 失败请求条数: 0
失败请求条数: 0
wangwei|iZ25lfbcwlvZ|~/tmp/async_redis_client
$ bash -x new_test.sh -api_kind=2 -conn_per_thread=1 -work_thread_num=1
+ ./new_test -conn_per_thread=10 -work_thread_num=2 -test_thread_num=10 -req_per_thread=10000 -api_kind=2 -conn_per_thread=1 -work_thread_num=1
Start use: 324082 ns, Join use: 90844 ns,

real	0m5.125s
user	0m2.144s
sys	0m1.624s
++ wc -l
++ grep 'ON REDIS REPLY,' /tmp/new_test.INFO
+ echo 总请求条数: 100000
总请求条数: 100000
++ wc -l
++ grep -v ', 1,'
++ grep 'ON REDIS REPLY,' /tmp/new_test.INFO
+ echo 失败请求条数: 0
失败请求条数: 0
```

