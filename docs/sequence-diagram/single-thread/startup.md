# 进程起来

单线程没有第二条线程。`Poll::Init` 在主线程上准备循环并挂上 tick。`StartWork` 进入主循环之后不再返回。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Cfg as Config
    participant Pool as ThreadPool
    participant Timer as BaseTimerManager
    participant Poll as Poll
    participant Log as Log
    participant App as 组件

    Main->>Cfg: Parse ../config/进程名.toml
    Main->>Pool: Init
    Main->>Timer: Init
    Note over Timer: 循环还没准备好，ArmTick 只记下 offset_ms 和 OnLoopTick
    Main->>Poll: Init
    Poll->>Poll: 记下主线程为 poll 线程
    Poll->>Poll: PrepareLoop
    Poll->>Poll: StartTickOnLoop
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
    Note over Main: SingleThreadStartWork 堵在主循环里
```

`ipcclient` 在 `StartClient` 之前，若开了 Redis 或 Hold，会先把 `RedisClient` 连上。HTTP 探测在 `ipcserver` 听上之后用 `StartTask` 拉起，不等它。句柄进单线程队列，要等主循环开始才会跑。
