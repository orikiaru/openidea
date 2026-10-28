# 处理并回包

处理协程在主线程的下一轮 `StScheduler::Poll` 里跑。业务当场结束时，调用方不挂起。回包在这次 `resume` 里写进发送缓冲，并当场打开写事件。字节要等这一轮后面的 `PollWait` 才 `send`。

`co_return` 不在 `final_suspend` 里销毁帧。`resume` 返回后 `DrainDetached` 再销毁。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Run as RunHandledRpc
    participant Biz as 业务任务
    participant Sess as ProtoSession
    participant Tcp as TCP_Channel

    Main->>Run: StScheduler.Poll 里 resume
    Run->>Run: 读出调用号并反序列化 Req
    Run->>Biz: co_await
    alt 业务当场结束
        Biz-->>Run: 回到这里，调用方不挂起
    else 业务自己挂起
        Biz-->>Run: 把调用方记到 continuation
        Note over Main: 业务结束时 Schedule，下一轮再回到 Run
    end
    Run->>Run: 包体 = 同一调用号 + Res
    Run->>Run: EncodePbFrame 回包协议号
    Run->>Sess: SendBytes
    Sess->>Tcp: AllowSend 当场打开写
    Run-->>Main: co_return
    Main->>Main: DrainDetached
    Note over Main: 这一轮随后 PollWait 里 DataOut 才 send
```

`ipcserver` 的 `AllEcho` 当场把请求抄进回包。`Hold` 在 `delay_ms > 0` 时先 `yield`，到点再回。
