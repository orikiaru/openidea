# 主循环

三条线同时走。主线程只等停止。poll 线程只做 `uv_run`。工人等 `normal` 队列里的任务或协程。协程 `resume` 之后才 `DrainDetached`，不在 `final_suspend` 里销毁帧；执行普通任务之后不调。

`queue` 工人单独一条，时间轮 `Update` 走它，不跟 `normal` 抢。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Poll as poll 线程
    participant Uv as libuv
    participant Worker as normal 工人

    Main->>Worker: StartWork 拉起工人
    Main->>Main: 等到 RequestStop
    par poll 一直转
        loop 直到 StopAndJoin
            Poll->>Uv: UV_RUN_ONCE
            Uv->>Poll: 可读 可写 或 tick
        end
    and 工人一直取
        loop 直到 Stopping
            alt 有协程
                Worker->>Worker: resume
                Worker->>Worker: DrainDetached
            else 只有任务
                Worker->>Worker: Exec 一个 Task
            end
        end
    end
    Main->>Worker: notify_all 然后 join
    Main->>Poll: StopAndJoin
```

`Schedule` 是 `PostCoroutine`：句柄放进 `normal` 队列，有空闲工人才 `notify_one`。
