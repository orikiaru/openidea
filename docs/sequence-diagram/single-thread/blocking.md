# 阻塞任务

业务在主线程上 `co_await RunBlocking`。前端不是协程：右值进闭包，再进入 `RunBlockingTask`。协程里调用时，`RunBlockingTask` 当场跑到 `BlockingAwaiter`。`await_suspend` 先记下这条任务的句柄，再 `SubmitBlocking`。

图里的 poll 就是主线程上的循环。`CallOnLoop` 发现已经在 poll 线程，当场 `SubmitOnLoop`。后台是该 `BlockingType` 的固定线程，用户函数在那里执行。完成靠本地 socketpair 回到 poll，`DataIn` 在挂起之后的 `PollWait` 里跑。

已关闭，或线程池已请求停止，`Finish` 带回 `OI_ERR_CONNECT`。本地会话已经关闭，也是 `OI_ERR_CONNECT`，发生在分配操作号之前。该类型在途数达到 `BlockingPoolCapacity`，带回 `OI_ERR_BUSY`。在途含排队、执行中和完成通知还没消费的工作；Default 的上限是线程数加队列上限，再与 1024 取较小值。这两种都在完成回调里把门写成 `Ready`。`exchange(Waiting)` 看见 `Ready`，`RunBlockingTask` 不挂起，当场 `co_return`。外层任务已经完成，业务的 `co_await` 看见 `bReady`，也不挂起。

未配置的非 `Default` 不在池表里，`BlockingPoolCapacity` 为 0，入场不按在途拒绝。`SubmitBlockingWork` 返回 `OI_ERR_NOT_FOUND`。池队列已满返回 `OI_ERR_BUSY`，池已停止返回 `OI_ERR_CONNECT`。第一次 `OpenPair` 或登记读端失败返回 `OI_ERR_SOCKET`。这些都撤销刚登记的待定项，再 `Finish` 该码，同样不挂起。提交过程抛出的异常用 `Finish(OI_OK, 异常)` 交回，`await_resume` 重抛。

接纳后在 poll 上分配操作号，跳过 0，也不占用仍在途的号。编一帧 `WorkCompleted`，协议号 `WORK_PROTOCOL_COMPLETED`，包体只有操作号，交给 `BlockingWorkChannel::Submit`。通道第一次打开本地 socketpair，读端挂到 poll。`SubmitBlockingWork` 把工作放进该类型的队列。不使用 `uv_queue_work`。

工作线程执行用户函数，异常收进 completion，然后 `Publish`。结果进本地就绪链表。链表从空变成非空时 `Wakeup` 写一个字节 `w`。字节不带业务数据。poll 读掉这个字节，取走整条链表，`SwapInBuff` 把预先编好的帧交给会话。`OnDataIn` 解 `WorkCompleted`。操作号必须和完成记录一致，然后 `Complete`。对不上或协议损坏也 `Complete`，带上当前异常，操作号用完成记录里的。

`Complete` 取出待定项并调用 `fnDone`。`fnDone` 写下状态码和异常。门从 `Waiting` 换成 `Ready` 才 `Schedule`。唤醒的是 `RunBlockingTask`。它在下一轮 `Poll` 里 `resume`：有异常就在 `await_resume` 重抛，否则状态码为 0 时 `co_return` 用户函数的 `int32_t`。业务由任务结束时再放进队列。这次 `Poll` 已经拿走队列，业务要再等一轮。丢弃 Task 不取消工作。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Biz as 业务协程
    participant Run as RunBlockingTask
    participant Gate as 完成门
    participant Poll as poll
    participant Chan as 阻塞通道
    participant Pool as 分类池
    participant Bg as 后台线程

    Main->>Biz: resume
    Biz->>Run: co_await RunBlocking
    Note over Run: 右值已进闭包。当场跑到 BlockingAwaiter
    Run->>Gate: await_suspend 记下 RunBlockingTask 的句柄
    Run->>Poll: SubmitBlocking，当场 SubmitOnLoop
    alt 已关闭或已请求停止
        Poll->>Gate: Finish，OI_ERR_CONNECT，门改成 Ready
        Note over Gate: 门还是 Idle，不 Schedule。本地会话已关闭时，分配操作号之前也是这个码
        Run->>Gate: exchange Waiting，看见 Ready
        Gate-->>Run: 不挂起
        Run->>Run: await_resume 无异常，co_return 该码
        Run-->>Biz: 任务已完成，外层 co_await 不挂起
    else 在途数达到 BlockingPoolCapacity
        Poll->>Gate: Finish，OI_ERR_BUSY，门改成 Ready
        Run->>Gate: exchange Waiting，看见 Ready
        Gate-->>Run: 不挂起
        Run-->>Biz: 外层不挂起，取到 OI_ERR_BUSY
    else 接纳
        Poll->>Poll: 没有本地会话就先建一条
        Poll->>Poll: 分配操作号，编 WorkCompleted
        Poll->>Chan: Submit
        alt 打开或登记读端失败
            Chan-->>Poll: OI_ERR_SOCKET
            Poll->>Gate: 撤销待定项，Finish 该码，门改成 Ready
            Run->>Gate: exchange Waiting，看见 Ready
            Gate-->>Run: 不挂起
            Run-->>Biz: 外层不挂起，原样返回该码
        else 读端已挂上
            Chan->>Pool: SubmitBlockingWork
            alt 没有入队
                Pool-->>Poll: 未配置 OI_ERR_NOT_FOUND，队列满 OI_ERR_BUSY，池已停 OI_ERR_CONNECT
                Poll->>Gate: 撤销待定项，Finish 该码，门改成 Ready
                Run->>Gate: exchange Waiting，看见 Ready
                Gate-->>Run: 不挂起
                Run-->>Biz: 外层不挂起，原样返回该码
            else 已入队
                Pool-->>Chan: OI_OK
                par 主线程挂起
                    Run->>Gate: exchange Waiting，看见 Idle
                    Gate-->>Run: RunBlockingTask 挂起
                    Run-->>Biz: 返回未完成的 Task
                    Biz->>Run: 登记到任务等待列表并挂起
                and 后台执行
                    Bg->>Pool: 取出这项
                    Bg->>Bg: 执行用户函数，异常收进 completion
                    Bg->>Chan: Publish。链表从空变成非空才写一个字节 w
                end
                Note over Chan: 字节不带业务数据。不使用 uv_queue_work
                Poll->>Chan: 之后 PollWait 里 DataIn，读掉字节，取走整条链表
                Chan->>Poll: SwapInBuff，OnDataIn 解 WorkCompleted
                alt 操作号和完成记录一致
                    Poll->>Gate: Complete 后 fnDone，写下状态码和异常
                else 协议损坏或操作号不一致
                    Poll->>Gate: Complete，带上异常，用完成记录里的操作号
                end
                Gate->>Main: 从 Waiting 换成 Ready 才 Schedule RunBlockingTask
                Main->>Run: 下一轮 Poll resume
                alt 有异常
                    Run->>Run: await_resume 重抛
                else 状态码是 0
                    Run->>Run: co_return 用户函数的 int32_t
                end
                Run->>Main: 结束时 Schedule 业务
                Note over Main: 这次 Poll 已拿走队列，业务留到再下一轮
                Main->>Biz: resume，取结果或再抛异常
            end
        end
    end
```

丢弃 Task 不取消已经进池的工作。提交过程里的 C++ 异常同样在这次 `SubmitOnLoop` 里 `Finish(OI_OK, 异常)`，`exchange(Waiting)` 看见 `Ready`，不挂起，异常在 `await_resume` 重抛。
