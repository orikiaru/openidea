# 客户端连上

`StartClient` 在主循环之前，由主线程发起。主线程就是 poll 线程，写事件当场挂上。连接完成要等 `StartWork` 里的 `PollWait`，完成回调也在主线程。

`SO_ERROR` 为 `EINPROGRESS` 或 `EINTR` 时通道留着，等下一次可写。Windows 上 `WSAEWOULDBLOCK`、`WSAEINPROGRESS` 会收成 `EINPROGRESS`。其它错误才 `AbortSession`，然后关掉这条连接通道。成功时 dup 一只 fd 交给会话，原 fd 随连接通道关掉。不能直接交原 fd：旧 uv_poll 关闭时会对这只 fd 做 epoll DEL，新会话的监听会被一起删掉。dup 失败按连接失败走 AbortSession。对端拒绝时 libuv 只回调错误、不报可写，还会停掉监听，所以 DataIn 里同样读 SO_ERROR 收尾。立刻失败时关掉 fd 并 AbortSession，走同一条重连。`connect` 立刻成功的路径同样交原 fd。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Conn as ConnectChannel
    participant Sock as NetSocket
    participant Os as NetOs
    participant Mgr as ProtoClientManager
    participant Sess as ProtoSession
    participant Queue as StScheduler

    Main->>Conn: StartConnect
    Conn->>Sock: CreateSocket 非阻塞 TCP
    Conn->>Sock: SocketConnect
    Sock->>Os: connect
    alt EINPROGRESS
        Sock->>Conn: new ConnectChannel
        Conn->>Main: 只注册写，当场 uv_poll_start
        Note over Main: StartWork 之后 PollWait 看到可写
        Main->>Conn: DataOut
        Conn->>Os: getsockopt SO_ERROR
        alt SO_ERROR 为 EINPROGRESS 或 EINTR
            Note over Conn: 不关通道，等下一次可写
        else SO_ERROR 为 0
            Conn->>Os: dup
            Conn->>Mgr: AddSession dup 出来的 fd
            Mgr->>Sess: CloneSession 并只注册读
            Mgr->>Mgr: 记下 sid，按序补发挂起的字节
            Mgr->>Queue: OnClientConnect 里的 StartTask 只入队
            Conn->>Conn: Close
            Main->>Conn: TryClose 后摘掉并 delete
        else SO_ERROR 为其它错误
            Conn->>Mgr: AbortSession
            Mgr->>Mgr: OnConnectFailed
            Conn->>Conn: Close
            Main->>Conn: TryClose 后摘掉并 delete
        end
    else connect 立刻成功
        Sock->>Mgr: AddSession 原 fd
        Mgr->>Queue: OnClientConnect 里的 StartTask 只入队
        Note over Queue: 主循环第一轮才会 resume
    else 其他错误
        Sock->>Os: close fd
        Sock->>Mgr: AbortSession
        Mgr->>Mgr: OnConnectFailed
        Sock-->>Main: StartClient 返回 false
    end
```

`ipcclient` 的 `OnClientConnect` 里再 `Start` 回声、日志和 Hold。
