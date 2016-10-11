## Versioning

This project follows the [semantic versioning](http://semver.org/) scheme. The API change and backwards compatibility rules are those indicated by SemVer.


## 是什么

AsyncRedisClient 异步 Redis 客户端. AsyncRedisClient 会启动 `thread_num` 个线程, 每个线程具有 `conn_per_thread` 个到指定 redis 实例(由 `host:port` 来指定)的连接. 当通过 `AsyncRedisClient::Execute()` 来执行请求时, AsyncRedisClient 会(通过 round-robin 算法)选择一个线程, 然后将请求交给该线程来进行处理, 线程内部会(通过 round-robin 算法)选择一个连接来处理该请求, 并且得到响应之后调用指定的回调函数.

从上看来, AsyncRedisClient 不支持事务这类与连接相关的命令, 虽然可以提供一个重载形式的 Execute(), 如下:

```cpp
Execute(AsyncRedisClient::Connection conn, request, callback);
```

表示着在指定的连接上执行 request. 但是通过 redis.io 得知, 事务完全可以用 lua 脚本然后在一个请求中实现. 因此就没有提供这种形式的 Execute().

## 怎么用

1.  创建, 并启动一个 AsyncRedisClient 实例, 一般情况下, 一个进程内只需要一个 AsyncRedisClient 实例即可. 如下:

    ```cpp
    AsyncRedisClient g_async_redis_cli;

    int main(int argc, char **argv) {
        g_async_redis_cli.conn_per_thread = FLAGS_conn_per_thread;
        g_async_redis_cli.thread_num = FLAGS_work_thread_num;
        g_async_redis_cli.host = FLAGS_redis_host;
        g_async_redis_cli.passwd = FLAGS_redis_passwd;
        g_async_redis_cli.port = FLAGS_redis_port;
        g_async_redis_cli.Start();
        // 之后就可以调用 g_async_redis_client.Execute() 来提交请求了.

    }
    ```

2.  通过 `AsyncRedisClient::Execute()` 系列 API 来提交请求, API 语义可以参考注释. 这里提供个栗子:

    ```cpp
    void OnRedisReply(redisReply *reply) noexcept {
        LOG(INFO) << "reply: " << *reply;
        return ;
    }

    // 同步方式.
    auto future_end = g_async_redis_cli.Execute(std::vector<std::string>{"SET", "hello", "world"});
    // future_end.wait_for(timeout); // 超时等待.
    auto reply = future_end.get();
    OnRedisReply(reply.get());

    // 异步方式.
    g_async_redis_cli.Execute(std::vector<std::string>{"GET", "hello"}, OnRedisReply);
    ```

3.  停止 AsyncRedisClient 实例, AsyncRedisClient 提供了 `AsyncRedisClient::Join()`, `AsyncRedisClient::Stop()` 用来停止实例, 区别可以参考注释.

### 依赖

1.  C++11
2.  [libuv, v1.9.1](https://github.com/libuv/libuv/tree/v1.9.1)
3.  [common, v1.1.0](https://github.com/pp-qq/common/tree/v1.1.0)
4.  [hiredis, v1.0.1](https://github.com/pp-qq/hiredis/tree/v1.0.1);

