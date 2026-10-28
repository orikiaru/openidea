# 客户端连上

`StartClient` 在主循环之前，由主线程发起。主线程就是 poll 线程，写事件当场挂上。连接完成要等 `StartWork` 里的 `PollWait`，完成回调也在主线程。

下面是当前代码。完成阶段有两处待改，行为先不动。

待改：

- `SO_ERROR` 为 `EINPROGRESS` 或 `EINTR` 时，分支是空的，后面仍调用 `AbortSession`。`DataOut` 接着无条件 `Close`，`ConnectChannel` 被摘掉，等不到下一次可写。`ipcclient` 的 `OnAbortSession` 是空的，这次失败不会重连。Windows 上 `NetOs_GetSocketError` 还会把 `WSAEWOULDBLOCK`、`WSAEINPROGRESS` 收成 `EINPROGRESS`。HTTP 客户端的 `OnWritable` 对这些值是留下再等。
- 成功时 `NetOs_Dup` 复制套接字再 `AddSession`，原 fd 留在 `ConnectChannel` 里，随后析构关掉。`dup` 失败得到无效句柄，`AddSession` 不检查。`connect` 立刻成功的那条路径是把原 fd 直接交出去的。异步成功也应交出这只套接字，不要复制。

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
        alt SO_ERROR 为 0
            Conn->>Mgr: AddSession dup fd
            Mgr->>Sess: CloneSession 并只注册读
            Mgr->>Mgr: 记下 sid，按序补发挂起的字节
            Mgr->>Queue: OnClientConnect 里的 StartTask 只入队
        else SO_ERROR 非 0
            Conn->>Mgr: AbortSession
            Mgr->>Mgr: OnConnectFailed 然后 OnAbortSession
        end
        Conn->>Conn: Close
        Main->>Conn: TryClose 后摘掉并 delete
    else connect 立刻成功
        Sock->>Mgr: AddSession 原 fd
        Mgr->>Queue: OnClientConnect 里的 StartTask 只入队
        Note over Queue: 主循环第一轮才会 resume
    else 其他错误
        Sock-->>Main: StartClient 返回 false
    end
```

`ipcclient` 的 `OnClientConnect` 里再 `Start` 回声、日志和 Hold。
