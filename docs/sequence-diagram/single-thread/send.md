# 发出一次调用

这是一条 `RpcCall`。按服务名选实例见 [按服务名调用](route.md)。选路之后这里只用 `RpcDeadline` 剩下的毫秒。`CallAll` 和 `CallMany` 对每一台先 `Submit`，把帧写进发送缓冲，再逐个 `co_await` 已经在途的等待器。

业务协程在主线程的 `StScheduler::Poll` 里跑。挂起前就把帧写进发送缓冲。因为这里就是 poll 线程，打开写事件是当场 `uv_poll_start`。真正 `send` 在这一轮后面的 `PollWait`。

回包、发送失败、超时，这三件事里只有先到的那一件会把协程放回队列。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Biz as 业务协程
    participant Call as RpcCall
    participant Wait as 等待表
    participant Sess as ProtoSession
    participant Tcp as TCP_Channel
    participant Os as NetOs

    Main->>Biz: StScheduler.Poll 里 resume
    Biz->>Call: co_await RpcCall
    opt 进程里第一次
        Call->>Main: 就在主线程 ArmRpcSweep
        Main->>Main: AddTimer 200ms 扫描
    end
    Call->>Wait: BindRpcCall 分配调用号并放入分片
    Call->>Call: 包体 = 调用号 + Req
    Call->>Call: EncodePbFrame 协议号 长度 包体
    Call->>Sess: SendBytes
    alt 找到会话
        Sess->>Sess: 追加 stOutputBuffer
        Sess->>Tcp: AllowSend
        Tcp->>Main: 当场打开写事件
    else 没有这条会话
        Call->>Wait: WakeRpcCall 结果 OI_ERR_FAIL
    end
    alt 回包或失败已经先到
        Call-->>Biz: 不挂起，主线程上 await_resume
    else 还在等
        Call-->>Biz: 挂起
        Note over Main: 这一轮协程批跑完后 PollWait
        Main->>Tcp: DataOut
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

解包在主线程的 `await_resume`。解出来返回 `OI_OK`，解包失败返回 `OI_ERR_FAIL`。没收到回包就返回等待表里已经写下的结果。
