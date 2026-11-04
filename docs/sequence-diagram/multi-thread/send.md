# 发出一次调用

这是一条 `RpcCall`。`CallAny` 和 `Call` 选好实例后走这里。`CallAll` 和 `CallMany` 对每一台先 `Submit`，把帧写进发送缓冲，再逐个 `co_await` 已经在途的等待器。

业务协程在 `normal` 工人上跑。挂起前就把帧写进发送缓冲。工人不是 poll 线程，打开写事件只是 `PostOp`。`send` 在 poll 线程的可写回调里。

回包、发送失败、超时，这三件事里只有先到的那一件会把协程放回队列。

```mermaid
sequenceDiagram
    autonumber
    participant Worker as normal 工人
    participant Biz as 业务协程
    participant Call as RpcCall
    participant Wait as 等待表
    participant Sess as ProtoSession
    participant Poll as poll 线程
    participant Tcp as TCP_Channel
    participant Os as NetOs

    Worker->>Biz: resume
    Biz->>Call: co_await RpcCall
    opt 进程里第一次
        Call->>Poll: CallOnLoop ArmRpcSweep
        Poll->>Poll: AddTimer 200ms 扫描
    end
    Call->>Wait: BindRpcCall 分配调用号并放入分片
    Call->>Call: 包体 = 调用号 + Req
    Call->>Call: EncodePbFrame 协议号 长度 包体
    Call->>Sess: SendBytes
    alt 找到会话
        Sess->>Sess: 追加 stOutputBuffer
        Sess->>Poll: AllowSend 变成 PostOp MOD
        Poll->>Tcp: uv_poll_start 打开写
    else 没有这条会话
        Call->>Wait: WakeRpcCall 结果 OI_ERR_FAIL
    end
    alt 回包或失败已经先到
        Call-->>Biz: 不挂起，工人上 await_resume
    else 还在等
        Call-->>Biz: 挂起，工人去干别的
        Poll->>Tcp: DataOut
        Tcp->>Sess: SwapOutBuff
        Tcp->>Os: send
        alt 缓冲写空
            Tcp->>Sess: SendDrained
            Sess->>Tcp: ForbidSend
        else EAGAIN
            Tcp->>Tcp: 留下未写完的，等下次可写
        end
    end
```

解包在工人的 `await_resume`。解出来返回 `OI_OK`，解包失败返回 `OI_ERR_FAIL`。没收到回包就返回等待表里已经写下的结果。
