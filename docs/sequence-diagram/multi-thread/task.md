# 任务创建即运行

在工人上的协程里直接调用协程函数，子任务的 `initial_suspend` 不挂起，当场在这个工人的栈上跑到第一次 `co_await`。当场跑完的，外层之后的 `co_await` 不再挂起，直接取已保存的结果。

协程外（网络回调、定时器回调）新建的任务，以及 `StartTask`、`TaskScope::Start`，`initial_suspend` 把句柄放进工人队列，不在 poll 回调里 `resume`。

同一个 `Task` / `TaskResult` 可以给多个协程等，任务只跑一次。完成后的等待者直接拿保存的结果或异常，不再入队。结果按值复制给每个等待者。

```mermaid
sequenceDiagram
    autonumber
    participant Worker as 工人
    participant Parent as 调用方协程
    participant Child as 子任务
    participant Queue as 工人队列

    alt 已经在工人协程里，并且不是 StartTask
        Worker->>Parent: resume
        Parent->>Child: 调用协程函数
        Child->>Child: initial_suspend 不挂起，在这条栈上跑
        alt 第一次挂起前就结束
            Child-->>Parent: Task 已完成
            Parent->>Parent: 之后 co_await 不挂起，直接取结果
        else 停在回包或 yield
            Child-->>Parent: 返回未完成的 Task
            Parent->>Child: co_await 登记到等待列表并挂起
            Child->>Queue: 结束时 PostCoroutine 每个等待者
            Worker->>Parent: 之后某个工人再 resume
        end
    else 回调，或 StartTask
        Parent->>Queue: initial_suspend 里 PostCoroutine
        Note over Queue: 不在 poll 回调里 resume
    end
```
