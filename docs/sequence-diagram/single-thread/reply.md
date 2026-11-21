# 处理并回包

处理协程在主线程的下一轮 `StScheduler::Poll` 里跑。处理函数可以返回 `int32_t`，当场调用；也可以返回 `RpcTask`，再 `co_await`。调用号 0 在业务结束后直接 `co_return`，不编码、不发回包。

普通回包的包体是去掉请求位的调用号，加上 int32 状态码，只有 0 才跟 Res。`EncodePbFrame` 用回包协议号。编码失败丢掉半包，改走 `SendFailureReply`。`SendFrame` 失败或发送抛异常就 `CloseSession`。TCP 上写事件当场打开，`send` 在这一轮后面的 `PollWait`。

`co_return` 不在 `final_suspend` 里销毁帧。`resume` 返回后 `DrainDetached` 再销毁。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Run as 包装
    participant Biz as 业务
    participant Sess as 会话
    participant Tcp as 通道

    Main->>Run: StScheduler.Poll 里 resume
    alt 处理函数返回 int32_t
        Run->>Biz: 当场调用
        Biz-->>Run: 带回状态码，这里不挂起
    else 处理函数返回 RpcTask
        Run->>Biz: co_await
        alt 业务当场结束
            Biz-->>Run: 回到包装，不挂起
        else 业务自己挂起
            Biz-->>Run: 登记到等待者
            Note over Main: 结束时 Schedule，下一轮再回到包装
        end
    end
    alt 调用号 0
        Run-->>Main: co_return，不编码、不发回包
        Main->>Main: DrainDetached
    else 普通回包
        Run->>Run: 包体 = 去掉请求位的调用号 + int32，只有 0 才跟 Res
        Run->>Run: EncodePbFrame，回包协议号
        alt 编码失败
            Run->>Run: 丢掉半包
            Run->>Sess: SendFailureReply
            opt 失败回执编不出、发不出去或抛异常
                Sess->>Sess: CloseSession
            end
            Run-->>Main: co_return
            Main->>Main: DrainDetached
        else 编码成功
            Run->>Sess: SendFrame
            alt 失败或抛异常
                Sess->>Sess: CloseSession
                Run-->>Main: co_return
                Main->>Main: DrainDetached
            else 写入发送缓冲
                Sess->>Tcp: AllowSend，当场打开写
                Run-->>Main: co_return
                Main->>Main: DrainDetached
                Note over Main: 这一轮后面的 PollWait 里 DataOut 才 send
            end
        end
    end
```

`ipcserver` 的 `AllEcho` 是同步处理，当场把 `stData` 抄进回包并写 `iCode = 1`。`Hold` 在 `iDelayMilliseconds > 0` 时先 `co_await OI::Coro::yield`，到点再回。负数延迟用业务码 `1001` 返回，回执不带响应体。
