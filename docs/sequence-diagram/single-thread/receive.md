# 收到一帧

读发生在主线程的 `PollWait` 里。一轮最多读 4MB。切出请求或回包之后只 `Schedule` 或 `StartTask`，协程留到这次 `uv_run` 返回之后的下一轮。

头或包体不够就留下，下一次可读再拼。

```mermaid
sequenceDiagram
    autonumber
    participant Main as 主线程
    participant Tcp as TCP_Channel
    participant Os as NetOs
    participant Sess as ProtoSession
    participant Rpc as RpcCallManager
    participant Wait as 等待表
    participant Queue as StScheduler

    Main->>Tcp: PollWait 里 DataIn
    loop 直到 EAGAIN 或满 4MB
        Tcp->>Os: recv
    end
    Tcp->>Sess: SwapInBuff
    Sess->>Sess: OnDataIn
    loop 缓冲里还能切出一帧
        alt 头或包体不够
            Sess->>Sess: 回滚，字节留在 stInputBuffer
        else 头非法
            Sess->>Sess: PassiveClose
        else 头合法
            Sess->>Rpc: Deliver 协议号和包体
            alt 是登记过的回包号
                Rpc->>Rpc: 读出调用号
                alt 等待表里还有这一号且协议号相符
                    Rpc->>Wait: FinishSlot 在 poll 线程解开回包
                    Wait->>Queue: Schedule
                    Note over Queue: 解包在这一轮 Deliver 里完成，下一轮才 resume
                else 已经取走或协议号不符
                    Rpc->>Rpc: 丢掉，或按 OI_ERR_FAIL 放回队列
                end
            else 是请求且注册了处理函数
                Note over Rpc: Deliver 的处理函数在 poll 线程解开请求
                alt 解开
                    Rpc->>Queue: StartTask 只入队
                    Note over Queue: 下一轮才跑处理协程
                else 解不开
                    Rpc->>Rpc: 调用号非 0 失败回执，调用号 0 不回包，不启动协程
                end
            else 没有等待者也没有处理函数
                Rpc->>Rpc: 丢掉这一包
            end
        end
    end
```

迟到的回包发现调用号已经取走，就丢掉，不会配给下一次调用。
