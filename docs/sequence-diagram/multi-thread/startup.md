# 进程起来

多线程里 poll 先单独转起来。`StartWork` 再拉工人，主线程自己不 poll，堵到 `RequestStop`。

条数读 `ThreadPool.threadcount`，必须配，并且大于 0。旧键 `normal` 仍会作为兼容读一次：读不到 `threadcount` 时会再读 `normal`，读到了就在 stderr 打一行提示改名。缺了或不是正数，`ThreadPool::Init` 返回 false，进程起不来。旧配置里的 `urgent`、`queue`、`max_task_count` 不再读取，留着不报错。时间轮的 `Update` 在 poll 线程上跑。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Cfg as Config
    participant Pool as ThreadPool
    participant Timer as BaseTimerManager
    participant Poll as poll 线程
    participant Log as Log
    participant App as 组件
    participant Worker as 工人

    Main->>Cfg: Parse ../config/进程名.toml
    Main->>Pool: Init
    Main->>Timer: Init
    Note over Timer: 循环还没准备好，ArmTick 只记下 offset_ms 和 OnLoopTick
    Main->>Poll: Init 另起线程
    Poll->>Poll: PrepareLoop
    Poll->>Poll: StartTickOnLoop
    Poll->>Poll: uv_run 开始转
    Poll-->>Main: g_loop_ready
    Main->>Log: Init
    Main->>App: Init
    alt 服务端
        App->>App: HandleRpc 登记处理函数
        App->>App: SessionManager.Init 读 ip port type
        Main->>App: StartServer
    else 客户端
        App->>App: SessionManager.Init 读 ip port type
        Main->>App: StartClient
    end
    Main->>Pool: StartWork
    Pool->>Worker: 按 threadcount 拉起工人
    Main->>Main: 等到 RequestStop
```

`ipcclient` 在 `StartClient` 之前，若开了 Redis 或 Hold，会先发起 `RedisClient` 建连。HTTP 探测在 `ipcserver` 听上之后用 `StartTask` 拉起。此时工人可能还没起来，句柄先留在协程队列里，工人起来后能取到。
