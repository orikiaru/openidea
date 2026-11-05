# 完成门

单次等待用 `coro::Gate`。RPC、注册表路由、Redis 协程调用和 HTTP 都是这一道门。`Task` 可以有多个等待者，走完成列表，不走这里。

状态上先记下 `stHandle`。结果字段在 `exchange(Ready)` 之前写完：RPC 是 `iResult` / `bReplied`，路由是 `iCode` / `vList`，Redis 是 `pReply` / `iStatus`，HTTP 是 `iStatus` / `sBody` / `bFailed`。门还是 `Idle` 时不 `Schedule`。第二次完成由各自的一次性标记挡住，RPC 摘走调用号，Redis 用 `iWon`，HTTP 用 `iDone`，不再改已经写好的结果。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Biz as 业务协程
    participant Wait as 等待状态
    participant Poll as poll
    participant Queue as StScheduler

    Main->>Biz: ResumeTask
    Biz->>Wait: co_await，await_ready 为假
    Biz->>Wait: 写下 stHandle，再提交
    alt 结果先写好
        Wait->>Wait: 写结果，exchange Ready
        Note over Wait: 门还是 Idle，不 Schedule
        Biz->>Wait: exchange Waiting，看见 Ready
        Wait-->>Biz: 不挂起
        Biz->>Biz: await_resume 当场取结果
    else 调用方先挂起
        Biz->>Wait: exchange Waiting，看见 Idle
        Wait-->>Biz: 挂起
        Poll->>Wait: 写结果，exchange Ready
        Wait->>Queue: 看见 Waiting，Schedule
        Main->>Biz: 下一轮 Poll 里 ResumeTask
        Biz->>Biz: await_resume
    end
```
