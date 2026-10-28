# 调用超时

超时不在 poll 线程上扫。tick 把 `Update` 丢给唯一的 `queue` 工人。扫到之后再 `PostCoroutine` 给 `normal` 工人，由工人 `resume` 并返回 `-2`。

扫描是 200 毫秒一次，按超时秒数分桶，只看每个桶的队头。

```mermaid
sequenceDiagram
    autonumber
    participant Poll as poll 线程
    participant Uv as libuv
    participant Timer as BaseTimerManager
    participant Queue as queue 工人
    participant Sweep as RpcSweepTimer
    participant Wait as 等待表
    participant Worker as normal 工人
    participant Biz as 业务协程

    Poll->>Uv: uv_run
    Uv->>Timer: OnLoopTick
    Timer->>Queue: AddTask Update
    Queue->>Timer: Update
    Timer->>Sweep: OnTimer
    Sweep->>Wait: 队头已到期且槽位还在
    Wait->>Wait: 取走槽位，结果写成 -2
    Wait->>Worker: PostCoroutine
    Worker->>Biz: resume
    Biz->>Biz: 没有回包，返回 -2
```

发送失败走同一条唤醒，结果是 `-1`。先到的那个算数，后到的 `TakeRpcSlot` 失败就不再动这条协程。
