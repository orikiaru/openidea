# 主循环

主线程每一轮按 `queue`、`urgent`、`normal` 的顺序跑任务。每个优先级轮到它时才记下当时的条数，只跑这么多。所以高优先级任务投给低优先级的任务这一轮就跑，同一优先级执行中再投的才留到下一轮。

任务跑完再 `StScheduler::Poll`。它拿走此刻的整条协程队列，任务里 `Schedule` 的协程也在这一轮 `resume`。`resume` 期间新排的协程进的是新队列，留到下一轮。

还有活就 `PollWait(0)`，一次都不堵。什么都没有才 `PollWait(1000)`。这里的 `1000` 只表示要堵住，`uv_run` 收到的是 `UV_RUN_ONCE`，不是 1000 毫秒。时间轮的 tick 每 `offset_ms`（默认 50 毫秒）会把它唤醒一次。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Queue as queue 任务
    participant Task as normal 任务
    participant St as StScheduler
    participant Poll as Poll
    participant Uv as libuv

    loop 直到 RequestStop
        Main->>Queue: 记下 queue 此刻的条数并执行
        Main->>Task: urgent 和 normal 轮到时再记条数并执行
        Main->>St: Poll
        St->>St: 拿走此刻的协程队列并 resume
        Note over St: resume 期间新排的协程留到下一轮
        St->>St: DrainDetached
        alt 还有任务或协程
            Main->>Poll: PollWait 0
            Poll->>Uv: UV_RUN_NOWAIT
        else 什么都没有
            Main->>Poll: PollWait 1000
            Poll->>Uv: UV_RUN_ONCE 堵到下一次事件或 tick
        end
    end
```

时间轮到点是在 `uv_run` 里把 `Update` 放进 `queue` 任务。轮子要等下一轮开头才转。
