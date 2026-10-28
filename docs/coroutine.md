# 协程

业务只用一条 `coro::Task`。单线程和多线程走同一个挂起、唤醒路径，差别在句柄放进哪条队列。

## 怎么写

要结果就 `co_await`，不等就 `coro::StartTask`。让出一段时间用 `co_await coro::yield(毫秒)`。

- `yield(ms)` 在 `ms > 0` 时进时间轮，到点再跑，等待期间不占 CPU
- `ms <= 0` 只把句柄放回队列，这一轮不再碰它。主循环会先去 poll，再跑新挂上的

`co_await` 一条 `Task` 会在调用方的栈上把子协程 `resume` 到它自己挂起为止。子协程当场跑完，调用方不挂起。子协程自己停住（等回包或定时）时，把调用方记在 `continuation` 上，子协程结束再 `Schedule` 调用方。

所以不要在第一次挂起之前层层 `co_await` 子任务，栈会随层数涨。`StartTask` 只入队，不加栈。

`co_return` 才结束一条 `coro::Task`。`task.Start()` 或 `StartTask` 把句柄交给调度器。调用方手里的临时 `Task` 析构不会释放正在跑的帧。

## 挂起

挂起只把句柄放回队列，不在回调里直接 `resume`。RPC 回包和超时也是 `Schedule`。

时间轮只能在 poll 线程上改。`yield` 经 `Poll::CallOnLoop` 到 poll 线程再 `AddTimer`，到点把句柄放回队列。

## 单线程和多线程

单线程主循环每一轮只跑进入时已经在队列里的普通 `Task`，再 `StScheduler::Poll` 只 `resume` 进入时已经在协程队列里的句柄。这一轮里 `yield(0)` 和 `StartTask` 进的是原队列，这次碰不到。然后如果还有待跑的任务或协程，`PollWait(0)` 只试一次网络；什么都没有才 `PollWait(1000)` 堵住等。

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

要在处理里等一段时间，直接 `co_await coro::yield(毫秒)`，不要占着工人空转。`Hold` 就是这样把回包推迟的。

序号和处理函数必须是协程参数。写在 lambda 捕获里不会进协程帧，函数返回后就是悬空引用。

## 客户端

连接建立后用 `StartTask` 拉起任务，任务里 `co_await RpcCall`。等待键是每次调用新分配的调用号，等待表按调用号分片加锁。回包、超时或发送失败只有一个会把这条协程 `Schedule` 回来。迟到的回包发现调用号已经取走，就丢掉，不会配给下一次调用。

超时单位是秒，默认 180，`RpcCall` 最后一个参数可改。它不挂时间轮，由一个 200 毫秒的周期定时器按时长分桶扫队头，所以精度是秒，不是毫秒。

```cpp
coro::StartTask([n](coro::Token) {
    return RunOne(n);
});

int ret = co_await RpcCall(mgr, sid, req, res);
```

`ret == 0` 是回包已解出，`-2` 是超时，`-1` 是发送失败或解包失败。
