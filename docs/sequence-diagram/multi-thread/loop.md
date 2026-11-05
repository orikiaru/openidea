# 主循环

三条线同时走。主线程只等停止。poll 线程只做 `uv_run`。工人只从协程队列取句柄并 `resume`。协程 `resume` 之后才 `DrainDetached`，不在 `final_suspend` 里销毁帧。

时间轮 `Update` 由 poll 线程在 tick 里直接执行，不经工人。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Poll as poll 线程
    participant Uv as libuv
    participant Worker as 工人

    Main->>Worker: StartWork 按 ThreadPool.normal 拉起工人
    Main->>Main: 等到 RequestStop
    par poll 一直转
        loop 直到 StopAndJoin
            Poll->>Uv: UV_RUN_ONCE
            Uv->>Poll: 可读 可写 或 tick
        end
    and 工人一直取协程
        loop 直到 Stopping
            Worker->>Worker: resume
            Worker->>Worker: DrainDetached
        end
    end
    Main->>Worker: Broadcast 然后 join
    Main->>Poll: StopAndJoin
```

`Schedule` 是 `PostCoroutine`：句柄放进协程队列，有空闲工人才 `Signal`。
