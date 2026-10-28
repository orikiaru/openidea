# 协程

业务只用一条 `coro::Task`。单线程和多线程走同一个挂起、唤醒路径，差别在句柄放进哪条队列。

## 启动和调用

`task.Start()` 或 `coro::StartTask` 把句柄交给调度器。调用方手里的临时 `Task` 析构不会释放正在跑的帧。

`co_await` 另一条 `Task` 就是调用它。子协程当场跑完，调用方不挂起。子协程自己停住时，把调用方记在 `continuation` 上，子协程结束再 `Schedule` 调用方。

`co_return` 才结束一条 `coro::Task`。

## 挂起

挂起只把句柄放回队列，不在回调里直接 `resume`。

- `coro::yield(ms)`：过若干毫秒再跑
- `coro::YieldToPool`：下一轮再跑
- `coro::Park` / `coro::Wake(uid)`：按任务 id 停住和唤醒。`StartTask` 生成的 `coro::Token` 就是这个 id
- RPC 回包和超时：同样 `Schedule`

时间轮只能在 poll 线程上改。`yield` 经 `Poll::CallOnLoop` 到 poll 线程再 `AddTimer`，到点把句柄放回队列。

## 单线程和多线程

单线程主循环先跑普通 `Task`。跑完后 `StScheduler::Poll` 把就绪协程 `resume` 完，队列空了才进 `poll`。

多线程由 `ThreadPool::PostCoroutine` 把句柄放进工人队列。工人只 `resume`，不在工人里销毁协程帧。根协程在 `final_suspend` 里 `DetachFinished`，`resume` 返回后 `DrainDetached` 再销毁。

## 服务端

`HandleRpc` 包一层：解包，`co_await` 业务任务，再把回包写出去。请求和回包的包体都以 8 字节调用号开头，客户端按调用号认回包，所以回包不用按接受顺序排队，处理完就发。

```cpp
HandleRpc<AllEchoReq, AllEchoRes>([](coro::Token, SessionManager*, sid_t, AllEchoReq& req, AllEchoRes& res) -> coro::Task {
    res.data = req.data;
    res.code = 1;
    co_return;
});
```

序号和处理函数必须是协程参数。写在 lambda 捕获里不会进协程帧，函数返回后就是悬空引用。

## 客户端

连接建立后用 `StartTask` 拉起任务，任务里 `co_await RpcCall`。等待键是每次调用新分配的调用号，等待表按调用号分片加锁。回包、超时（默认 180 秒，`RpcCall` 最后一个参数可改）或发送失败都把这条协程 `Schedule` 回来。超时不挂时间轮，由一个周期定时器按时长分桶扫队头。

```cpp
coro::StartTask([n](coro::Token token) {
    return RunOne(token, n);
});

int ret = co_await RpcCall(mgr, sid, req, res);
```
