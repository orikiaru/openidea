# 让出

空等用 `co_await yield(毫秒)`。业务协程在工人上，时间轮只能在 poll 线程上改。`毫秒 > 0` 时 `CallOnLoop` 到 poll 线程再 `AddTimer`。到点由 `queue` 工人执行 `OnTimer`，再把句柄放回 `normal` 工人，不在定时器回调里 `resume`。

`毫秒 <= 0` 不进时间轮，直接 `PostCoroutine`。

```mermaid
sequenceDiagram
    autonumber
    participant Worker as normal 工人
    participant Biz as 业务协程
    participant Yld as yield
    participant Poll as poll 线程
    participant Timer as BaseTimerManager
    participant Queue as queue 工人

    Worker->>Biz: resume
    Biz->>Yld: co_await yield
    alt 毫秒小于等于 0
        Yld->>Worker: PostCoroutine
        Yld-->>Biz: 挂起，这个工人接着干别的
        Worker->>Biz: 之后另一个或同一个工人再 resume
    else 毫秒大于 0
        Yld->>Poll: CallOnLoop
        Poll->>Timer: AddTimer
        Yld-->>Biz: 挂起
        Note over Poll: 之后 tick 把 Update 丢给 queue 工人
        Queue->>Timer: Update
        Timer->>Worker: OnTimer 里 PostCoroutine
        Worker->>Biz: resume
    end
```
