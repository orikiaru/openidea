# 客户端连上

`StartClient` 在主线程发起。poll 从 `Poll::Init` 起就在转。`BeginConnect` 不在 poll 线程，`CallOnLoop` 投递 `IO_OP_CALL`，主线程不等。poll 线程 `Drain` 后 `StartConnect`。写事件在 poll 线程当场挂上，可写可能赶在 `StartWork` 之前。返回的是可等待对象，不等待也会发起连接。字面地址在 `Resolve` 里当场回调；域名走 `uv_getaddrinfo`，回调仍在 poll 线程。

`EINPROGRESS` 新建 `ConnectChannel`，只注册写。`connect` 立刻返回 0 时，原 fd 交给 `CompleteConnect`，不 dup。`OnAddSession` 里的 `StartTask` 放进工人队列。工人已经在跑时，可以在回调返回前 resume；还没 `StartWork` 时句柄先留在队列里。

`SO_ERROR` 为 `EINPROGRESS` 或 `EINTR` 时，`DataOut` 留着等下一次可写。Windows 上 `WSAEWOULDBLOCK`、`WSAEINPROGRESS` 收成 `EINPROGRESS`。其它错误才 `AbortSession`，然后关掉这条连接通道。成功时 `CheckTcpConnect` 里 dup，原 fd 留给连接通道关掉。不能把已经挂上 poll 的原 fd 直接交给新会话：Linux 上旧 uv_poll 关闭时会 epoll DEL，把新会话的监听一起删掉；Windows 走同一条 dup，不把这个原 fd 交出去。dup 失败走 `AbortSession`。`DataIn` 同样读 `SO_ERROR`，不论结果都 `Close`：libuv 遇到错误会停掉 poll，可写不会再来；这时若还在连，要先 `AbortSession`。`CompleteConnect` 校验代次，代次不符、这一代已经结束或服务已下线，就关掉交过来的 fd。`AddSession` 没建成会话时不进 `OnAddSession`，等待者拿到它的错误码，再 `OnConnectFailed`。

立刻失败时关掉 fd 并 `AbortSession`。`OnConnectFailed` 在允许自动重连时再定时 `StartClient`。解析失败且这一代还挂着，也走 `AbortSession`。已经连上同一端点就直接成功，不再 `connect`。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Conn as ConnectChannel
    participant Sock as NetSocket
    participant Os as NetOs
    participant Poll as poll 线程
    participant Mgr as ProtoClientManager
    participant Sess as ProtoSession
    participant Worker as 工人

    Main->>Mgr: StartClient 返回可等待对象，不等待也发起
    Mgr->>Poll: CallOnLoop 投递 IO_OP_CALL，主线程不等
    Poll->>Mgr: Drain 后 StartConnect
    Mgr->>Mgr: Resolve 后 ConnectChannel::StartConnect
    Conn->>Os: CreateTcp 非阻塞
    Conn->>Sock: SocketConnect
    Sock->>Os: connect
    alt EINPROGRESS
        Sock->>Conn: new ConnectChannel
        alt 只注册写成功
            Conn->>Poll: 已在 poll 线程，当场 uv_poll_start
            alt 可写进 DataOut
                Poll->>Conn: DataOut
                Conn->>Os: getsockopt SO_ERROR
                alt EINPROGRESS 或 EINTR
                    Note over Conn: 不关，等下一次可写
                else SO_ERROR 为 0
                    Conn->>Os: dup
                    Conn->>Mgr: CompleteConnect
                    alt 代次不符、这一代已结束或服务已下线
                        Mgr->>Os: 关掉 dup 出来的 fd
                    else 代次相符
                        Mgr->>Sess: CloneSession，只注册读
                        Mgr->>Mgr: 记下 sid，按序补发挂起的字节和调用
                        Mgr->>Worker: OnClientConnect 里 PostCoroutine
                        Note over Worker: 有空闲工人时，回调返回前就可能 resume
                        Note over Mgr: 再用 AddSession 的返回码唤醒等待者
                    end
                    Conn->>Conn: Close
                    Poll->>Conn: TryClose 摘掉并 delete，原 fd 随 uv_poll 关闭
                else 其它错误或 dup 失败
                    Conn->>Mgr: AbortSession
                    Mgr->>Mgr: OnConnectFailed
                    Conn->>Conn: Close
                    Poll->>Conn: TryClose 摘掉并 delete
                end
            else libuv 错误进 DataIn
                Poll->>Conn: DataIn
                Conn->>Os: getsockopt SO_ERROR
                alt 还在连
                    Conn->>Mgr: AbortSession
                else SO_ERROR 为 0
                    Conn->>Os: dup 后 CompleteConnect，不符关 fd，相符就建会话
                else 失败或 dup 失败
                    Note over Conn: CheckTcpConnect 里已经 AbortSession
                end
                Conn->>Conn: 不论结果都 Close
                Poll->>Conn: TryClose 摘掉并 delete
            end
        else 注册写失败
            Note over Conn: delete 时析构关掉尚未注册的 fd，再 AbortSession
            Conn->>Mgr: AbortSession，带上注册错误码
            Mgr->>Mgr: OnConnectFailed
        end
    else connect 立刻返回 0
        Sock->>Mgr: CompleteConnect，原 fd，不 dup
        alt 代次不符、这一代已结束或服务已下线
            Mgr->>Os: 关掉原 fd
        else 代次相符
            Mgr->>Sess: CloneSession，只注册读
            Mgr->>Mgr: 记下 sid，按序补发挂起的字节和调用
            Mgr->>Worker: OnClientConnect 里 PostCoroutine
            Note over Worker: 工人还没拉起时句柄先留在队列里
        end
    else 其它错误
        Sock->>Os: close fd
        Sock->>Mgr: AbortSession
        Mgr->>Mgr: OnConnectFailed
        Note over Mgr: 等待者拿到 int32_t 错误码
    end
```

`ipcclient` 的 `OnClientConnect` 里再拉起回声、协程、空消息、日志、Hold 和数据库测试，进程内只做一次。

`StartClient()` 不等待也会启动连接；`co_await StartClient()` 等待同一代连接的结果，成功返回 `OI_OK`，失败返回对应错误码。回调先于 await 到达时保留结果，后续 await 不丢唤醒；无布尔模式参数。
