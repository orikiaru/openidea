# 发出一次调用

这是一条 `RpcCall`。按服务名选实例见 [按服务名调用](route.md)。选路之后这里只用 `RpcDeadline` 剩下的毫秒。`CallAll` 和 `CallMany` 对每一台先 `Submit`，把帧写进发送缓冲，再逐个 `co_await` 已经在途的等待器。

业务协程在工人上跑。`CheckThreadAccess` 是空的，工人可以把帧追进发送缓冲。显式 sid 直接 `SendFrame`。按服务名或客户端对象发出时带 `RpcSender`，走 `SubmitCall`：已经连上再 `SendFrame`；还没连过就暂存，连上后 `FlushHeldCalls`；已经连过再断线是 `OI_ERR_CONNECT`；暂存超限是 `OI_ERR_BUSY`。打开写事件时 `ChangeEvents` 不在 poll 线程，投 `IO_OP_MOD`。`send` 在 poll 线程的可写回调里。

回包、发送失败、超时，这三件事里只有先到的那一件会 `Schedule`。还没连上就到期是 `OI_ERR_CONNECT`，已经发出后到期是 `OI_ERR_TIMEOUT`。

```mermaid
sequenceDiagram
    autonumber
    participant Worker as 工人
    participant Biz as 业务协程
    participant Call as RpcCall
    participant Wait as 等待表
    participant Mgr as SessionManager
    participant Sess as ProtoSession
    participant Poll as poll 线程
    participant Tcp as TCP_Channel
    participant Os as NetOs

    Worker->>Biz: resume
    Biz->>Call: co_await RpcCall
    Note over Call: CheckThreadAccess 是空的
    Call->>Wait: Bind 分配调用号，去掉 RPC_REQ_MASK，放进分片
    Call->>Call: 包体 = 调用号或上 RPC_REQ_MASK，再序列化 Req
    Call->>Call: EncodePbFrame 协议号 长度 包体
    alt 组包抛异常
        Call->>Wait: Wake OI_ERR_FAIL
        Note over Call: await_resume 重抛
    else 组包成功
        alt 显式 sid
            Call->>Mgr: SendFrame
        else 带 RpcSender
            Call->>Mgr: SubmitCall
        end
        alt 还没连过，SubmitCall 暂存
            Note over Mgr: 连上后 FlushHeldCalls，这次不发送
        else 连过又断，或找不到会话、已关闭
            Mgr-->>Call: OI_ERR_CONNECT
            Call->>Wait: Wake 这个码
        else 队列或暂存超限
            Mgr-->>Call: OI_ERR_BUSY
            Call->>Wait: Wake 这个码
        else 写入发送缓冲
            Mgr->>Sess: SendBuffered 追加 stOutputBuffer
            Sess->>Tcp: AllowSend
            Tcp->>Poll: ChangeEvents 投 IO_OP_MOD
            Poll->>Tcp: Drain 里 uv_poll_start 打开写
        end
    end
    alt 结果已经写好
        Call-->>Biz: 不挂起，工人上 await_resume 取已写下的结果
    else 还在等
        Call-->>Biz: 挂起，工人去干别的
        Poll->>Tcp: 可写回调里 DataOut
        Tcp->>Sess: SwapOutBuff
        Tcp->>Os: send
        alt 缓冲写空
            Tcp->>Sess: SendDrained
            Sess->>Tcp: 会话缓冲也空则 ForbidSend
        else EAGAIN
            Tcp->>Tcp: 留下未写完的，等下次可写
        else 硬错误
            Tcp->>Sess: PassiveClose
        end
    end
```

回包在 poll 线程的 `DecodeReply` 里解开，由 `FinishSlot` 调用，不在 `await_resume`。状态码为 0 时，`await_resume` 把已解出的对象移进调用方输出。解不开时码是 `OI_ERR_FAIL`，对象不移，异常只报告，不重抛。没收到回包就返回等待表里已经写下的码。

组包抛异常才 `Wake(OI_ERR_FAIL)` 并在 `await_resume` 重抛。槽已经被取走时，这次 `Wake` 不再改结果，异常仍重抛。poll 线程可以在工人 `Arm` 之前把回包解开并写好结果，这时不挂起，也不 `Schedule`。调用若已经在 poll 线程，`ChangeEvents` 当场打开写，不投 `IO_OP_MOD`。同一次还没取走时，后面的开关并进这一次 `IO_OP_MOD`。

`DataOut` 在 poll 线程，关写当场做。`EINTR` 再写一次。写空之后会话缓冲又有数据，就不 `ForbidSend`。硬错误 `PassiveClose` 之后关掉会话，还在表里的调用按 `OI_ERR_CONNECT` 结束。带对端地址的 UDP 不走这条 `DataOut`。`IO_OP_MOD` 之后哪一轮 `uv_run` 才进 `DataOut`，图里不区分。
