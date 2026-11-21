# UDP

收发都在 poll 线程。单线程的 poll 就是主线程。`OpenPeer` 和打开写事件当场完成。别的线程进来返回 `OI_ERR_WRONG_THREAD`。

## 绑定并建对端

`StartServer` 经 `StartAccept` 的 UDP 分支，`StartClient` 经 `StartConnect` 的 UDP 分支，都进 `UDP_Channel::StartBind`。已经有绑定通道时，`StartClient` 直接成功，不再绑一次。只绑本端，不指定远端，不开 `SO_REUSEADDR`。端口 0 用 `getsockname` 得到的端口，`LocalPort` 可读。绑定通道只注册读，会话还没有。

`OpenPeer` 只认字面地址。还没绑定返回 `OI_ERR_CONNECT`，地址非法返回 `OI_ERR_INVALID_ARGUMENT`。失败不改输出 sid。同一端点已有未关闭会话就复用。否则 dup 绑定套接字，新建会话和一条只用于发送的 `UDP_Channel`，源端口一致。dup 失败返回空，`OpenPeer` 返回 `OI_ERR_SOCKET`。

空闲定时器挂在绑定上。`idle_timeout_ms` 小于 4000 时，周期取它的四分之一，至少 1 毫秒，否则 1000 毫秒。回调是 `CloseIdleUdpPeers`。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Bind as 绑定通道
    participant Os as NetOs
    participant Mgr as SessionManager
    participant Peer as 发送通道

    Main->>Bind: StartBind
    Bind->>Os: 非阻塞 UDP socket
    Bind->>Os: bind 本端
    Note over Os: 不指定远端，不开 SO_REUSEADDR
    Bind->>Os: getsockname
    Note over Bind: 端口 0 记下分配到的端口，LocalPort 可读
    Bind->>Main: 只注册读，当场 uv_poll_start
    Note over Bind: 会话还没有
    Bind->>Main: uv_timer
    Note over Main: idle 小于 4000 取 idle/4，至少 1，否则 1000 毫秒，回调 CloseIdleUdpPeers
    Note over Main: 绑定返回之后，要发给哪个对端再 OpenPeer

    Main->>Mgr: OpenPeer 字面地址
    alt 还没绑定
        Mgr-->>Main: OI_ERR_CONNECT，不改 sid
    else 地址非法
        Mgr-->>Main: OI_ERR_INVALID_ARGUMENT，不改 sid
    else 同一端点已有未关闭会话
        Mgr-->>Main: 复用，写下 sid
    else 新建
        Mgr->>Os: dup 绑定套接字
        alt dup 失败
            Mgr-->>Main: OI_ERR_SOCKET，不改 sid
        else dup 成功
            Mgr->>Peer: 新建会话和只用于发送的通道
            Note over Peer: 源端口一致，不注册读
            Mgr-->>Main: 写下 sid
        end
    end
```

`CloseIdleUdpPeers` 用 `SteadyMilliseconds` 比 `idle_timeout_ms`，默认 60000。先退出对端表的锁，再拿会话锁复查。复查不过就留下。仍到期且未关闭，标 `bPeerIdleExpired`，再 `PassiveClose(ETIMEDOUT)`。`Close` 里 `AllowSend` 看见会话已不是连接中，清掉待发包并退额度，然后 `PostOnLoop`。下一轮 poll 才 `RecycleUdpPeer`：只摘自己那一项，再删掉发送通道。新会话已经顶掉同一端点时，不删新项。已经标过期再入队，直接 `OI_ERR_CONNECT` 并退额度。

## 收一个数据报

只有绑定通道的 `DataIn` 调 `recvfrom`。一轮最多 64 个数据报。收包缓冲按 64K 预留，按实际长度拷一份交给会话。`EINTR` 继续收。空包、地址非法，或 `OpenUdpPeerLocked` 返回空，丢掉这一包，绑定通道还在。

按来源地址 `OpenUdpPeerLocked`。已有未关闭会话就复用，否则 dup 再建。拿到会话就更新活跃时间并 `SwapInBuff`。这一包大于剩余接收额度时，`SwapInBuff` 清掉它并 `PassiveClose(ENOBUFS)`。这和后面切帧失败不关会话不是同一条。

之后走 `ProtoSession::OnDataIn` 的数据报分支。这一数据报里能切出的完整帧交给 `Deliver`。剩余字节清掉，不拼到下一包。坏帧不 `PassiveClose`。单线程只 `Schedule` 或 `StartTask` 入队，下一轮 `Poll` 才 `resume`。之后与 TCP 收帧同一条 `Deliver`。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Bind as 绑定通道
    participant Os as NetOs
    participant Mgr as SessionManager
    participant Sess as ProtoSession
    participant Rpc as RpcCallManager
    participant Queue as StScheduler

    Main->>Bind: PollWait 里 DataIn
    Bind->>Bind: 收包缓冲预留 64K
    loop 一轮最多 64 个
        Bind->>Os: recvfrom
        alt EAGAIN 或其他错误
            Bind-->>Main: 这一轮结束
        else 拿到数据报
            Bind->>Bind: 按实际长度拷一份
            Bind->>Mgr: 按来源地址 OpenUdpPeerLocked
            alt 返回空
                Bind->>Bind: 丢掉这一包
            else 拿到会话
                Mgr->>Sess: 更新活跃时间
                Bind->>Sess: SwapInBuff
                Sess->>Sess: OnDataIn 数据报分支
                loop 这一包里还能切出完整帧
                    Sess->>Rpc: Deliver
                    Note over Rpc: 之后与 TCP 收帧同一条 Deliver
                    Rpc->>Queue: Schedule 或 StartTask 只入队
                end
                Sess->>Sess: 剩余字节清掉
                Note over Sess: 不拼到下一包，坏帧不 PassiveClose
                Note over Queue: 下一轮 Poll 才 resume
            end
        end
    end
```

## 发一个数据报

业务按 sid 发。帧走 `QueuePacket(true)`：相邻帧可合并，目标 1200 字节；超过目标的单帧仍单独成包。队尾若是原始数据，下一帧也不并上去。原始数据走 `AllowSend`，再到 `QueuePacket(false)`，不合并。

IPv4 超过 65507、IPv6 超过 65527，立即拒绝。字节已经从发送缓冲取走的，`SendBuffered` 退还额度。队列从空变成非空时，给绑定通道当场打开写事件。

`DataOut` 在绑定通道上。先撤写关注，拷贝对端表，再逐个 `Flush`。`Flush` 对该对端的 dup 套接字 `sendto`，一轮最多 8 个包。`EAGAIN` 留下队首，这一轮扫完后重新打开写事件。整包发出才 `OnBytesSend`，并刷新活跃时间。硬错误或没有整包发出：丢弃该包，退额度，不重发，不调 `OnBytesSend`。入队成功不表示对端已收到。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Sess as 会话
    participant Peer as 发送通道
    participant Bind as 绑定通道
    participant Os as NetOs

    Main->>Sess: 按 sid 发
    alt 帧
        Sess->>Peer: QueuePacket(true)
        Note over Peer: 相邻帧可合并，目标 1200 字节。超过目标的单帧仍单独成包
    else 原始数据
        Sess->>Peer: AllowSend 再到 QueuePacket(false)
        Note over Peer: 不合并
    end
    alt 超过 IPv4 65507 或 IPv6 65527
        Peer-->>Sess: 立即拒绝，退还已取走的额度
    else 已经入队
        opt 队列从空变成非空
            Peer->>Bind: 当场打开写事件
        end
        Note over Peer: 并进已有的包时，写事件已经开着
        Note over Sess,Os: 入队成功不表示对端已收到
        Note over Main: 这一轮后面的 PollWait
        Main->>Bind: DataOut
        Bind->>Bind: 先撤写关注，拷贝对端表
        loop 每个对端
            Bind->>Peer: Flush
            loop 直到 8 个包、队列空或 EAGAIN
                Peer->>Os: 对这条 dup 套接字 sendto
                alt 整包发出
                    Peer->>Sess: OnBytesSend，刷新活跃时间
                else EAGAIN
                    Note over Peer: 留下队首，结束这个对端
                else 硬错误或没有整包发出
                    Note over Peer: 丢弃该包，退额度，不重发，不调 OnBytesSend
                end
            end
        end
        opt 还有没发完的
            Bind->>Main: 重新打开写事件，当场 uv_poll_start
        end
    end
```
