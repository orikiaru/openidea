# 进程起来

多线程里 poll 先单独转起来。`StartWork` 再拉工人，主线程自己不 poll，堵到 `RequestStop`。

条数读 `ThreadPool.normal`、`ThreadPool.urgent`、`ThreadPool.queue`，三项都必须配。缺一项 `ThreadPool::Init` 就返回 false，进程起不来。`queue` 读到后强制改成 1 条，时间轮的 `Update` 走这条。

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
    Pool->>Worker: normal urgent queue 各拉起自己的条数
    Main->>Main: 等到 RequestStop
```

`ipcclient` 在 `StartClient` 之前，若开了 Redis 或 Hold，会先把 `RedisClient` 连上。HTTP 探测在 `ipcserver` 听上之后用 `StartTask` 拉起。此时工人可能还没起来，句柄先留在 `normal` 队列里，工人起来后谓词里能看到。
