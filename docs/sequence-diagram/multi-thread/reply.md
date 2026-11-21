# 处理并回包

处理协程在工人上跑。业务当场结束时，调用方不挂起。回包在工人里写进发送缓冲，写事件交给 poll 线程。`send` 不在工人上。

`co_return` 不在 `final_suspend` 里销毁帧。工人 `resume` 返回后 `DrainDetached` 再销毁。

```mermaid
sequenceDiagram
    autonumber
    participant Worker as 工人
    participant Run as RpcHandlerBuilder::Run
    participant Biz as 业务任务
    participant Sess as ProtoSession
    participant Poll as poll 线程
    participant Tcp as TCP_Channel
    participant Os as NetOs

    Worker->>Run: resume
    Run->>Biz: co_await
    alt 业务当场结束
        Biz-->>Run: 回到这里，调用方不挂起
    else 业务自己挂起
        Biz-->>Run: 把调用方记到 continuation
        Note over Worker: 业务结束时 PostCoroutine，工人再回到 Run
    end
    Run->>Run: 包体 = 调用号 + int32 状态码，只有 0 才跟 Res
    Run->>Run: EncodePbFrame 回包协议号
    Note over Run: 编码失败走 SendFailureReply
    Run->>Sess: SendBytes
    Sess->>Poll: AllowSend 变成 PostOp MOD
    Run-->>Worker: co_return
    Worker->>Worker: DrainDetached
    Poll->>Tcp: DataOut
    Tcp->>Os: send
```

`ipcserver` 的 `AllEcho` 是同步处理，当场把 `stData` 抄进回包并写 `iCode = 1`。`Hold` 在 `iDelayMilliseconds > 0` 时先 `co_await OI::Coro::yield`，到点再回。负数延迟用业务码 `1001` 返回，回执不带响应体。
