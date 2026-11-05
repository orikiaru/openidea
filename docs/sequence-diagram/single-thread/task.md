# 任务创建即运行

在协程里直接调用协程函数，子任务的 `initial_suspend` 不挂起，当场在调用方栈上跑到第一次 `co_await`。当场跑完的，外层之后的 `co_await` 不再挂起，直接取已保存的结果。

协程外（`main`、网络回调、定时器回调）新建的任务，以及 `StartTask`、`TaskScope::Start`，`initial_suspend` 把句柄放进单线程队列。这一轮 `Poll` 已经把队列拿走，新句柄要等下一轮。

同一个 `Task` / `TaskResult` 可以给多个协程等，任务只跑一次。完成后的等待者直接拿保存的结果或异常，不再入队。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Parent as 调用方协程
    participant Child as 子任务
    participant Queue as StScheduler

    alt 已经在协程里，并且不是 StartTask
        Main->>Parent: resume
        Parent->>Child: 调用协程函数
        Child->>Child: initial_suspend 不挂起，在这条栈上跑
        alt 第一次挂起前就结束
            Child-->>Parent: Task 已完成
            Parent->>Parent: 之后 co_await 不挂起，直接取结果
        else 停在回包或 yield
            Child-->>Parent: 返回未完成的 Task
            Parent->>Child: co_await 登记到等待列表并挂起
            Child->>Queue: 结束时 Schedule 每个等待者
            Main->>Parent: 下一轮 Poll 再 resume
        end
    else main、回调，或 StartTask
        Parent->>Queue: initial_suspend 里 Schedule
        Note over Queue: 这一轮碰不到，下一轮 Poll 再 resume
    end
```
