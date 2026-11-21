# 收到一帧

读发生在 poll 线程的 `uv_run` 里。一轮最多读 4MB，还要看会话的 `ReceiveAllowance`。通道缓冲顶到额度就先 `SwapInBuff` 交给解析；解析后仍无空间，会话没关，读事件还在，就 `PassiveClose(ENOBUFS)`。

切帧在 `OnDataIn`。先读 `uint64` 调用号：0 是单向消息，请求位是 `RPC_REQ_MASK`。回包要 `TakeSlot` 同时对上管理器、sid 和回包协议号，对上才在 poll 线程 `DecodeReply`，调用方已经挂起才 `Schedule`。请求在 poll 线程解开后才 `StartTask`。这两处都只入队，不在 poll 回调里 `resume`。

TCP 头或包体不够就回滚，字节留在输入缓冲，同一残包只挂一次计时。数据报不走这条 `recv`，和 TCP 共用切帧，剩余字节不拼到下一包。

```mermaid
sequenceDiagram
    autonumber
    participant Poll as poll 线程
    participant Tcp as 通道
    participant Os as 套接字
    participant Sess as 会话
    participant Rpc as 分发
    participant Wait as 等待
    participant Worker as 工人

    Poll->>Tcp: uv_run 里 DataIn
    loop 未顶到额度，本轮未满 4MB，且还能读
        Tcp->>Os: recv，不超过额度、空闲和本轮剩余
    end
    Note over Tcp: EAGAIN、对端关闭或出错就停；EINTR 再读。顶到额度这一轮不再读
    Tcp->>Sess: SwapInBuff
    Note over Sess: 下面是 OnDataIn。数据报会话也进这里
    loop 缓冲里还能看下一帧
        alt 声明长度超过 max_frame_bytes
            alt TCP
                Sess->>Sess: PassiveClose(EMSGSIZE)
            else 数据报
                Note over Sess: 不关连接
            end
            Note over Sess: 结束这一轮切帧
        else 头或包体不够
            Sess->>Sess: 回滚，字节留在输入缓冲
            Note over Sess: 结束这一轮切帧
        else 头不是 OK
            alt TCP
                Sess->>Sess: PassiveClose(PARSER_ERR_OTHER)
            else 数据报
                Note over Sess: 不关连接
            end
            Note over Sess: 结束这一轮切帧。长度最高位为 1 时走上面的超限
        else 头准备成功
            Sess->>Sess: 取消残包计时
            alt Deliver 抛出未接住的异常
                alt TCP
                    Sess->>Sess: PassiveClose(PARSER_ERR_OTHER)
                else 数据报
                    Note over Sess: 不关连接
                end
                Note over Sess: 结束这一轮切帧
            else 进入分发
            Sess->>Rpc: Deliver，协议号和包体
            Rpc->>Rpc: 读 uint64 调用号
            alt 读不出调用号
                Rpc->>Rpc: 丢掉这一帧，接着切
            else 调用号 0，或带请求位
                alt 没有处理函数
                    Rpc->>Rpc: 丢掉，不关连接
                else 拿不到入口租约
                    alt 单向消息
                        Rpc->>Rpc: 丢掉
                    else 普通请求
                        Rpc->>Sess: 用这一帧的协议号回 OI_ERR_BUSY
                        opt 发不进去
                            Rpc->>Sess: CloseSession
                        end
                    end
                else 拿到租约，poll 线程解请求
                    alt 解不开
                        alt 调用号非 0
                            Rpc->>Sess: SendFailureReply，只有调用号和错误码
                            opt 发不出去或抛异常
                                Rpc->>Sess: CloseSession
                            end
                        else 调用号 0
                            Rpc->>Rpc: 不回包
                        end
                        Note over Rpc: 都不 StartTask
                    else 解开了
                        Rpc->>Worker: StartTask，只入队
                        Note over Worker: 不在这次 poll 回调里 resume
                    end
                end
            else 回包
                Rpc->>Wait: TakeSlot，对管理器、sid、回包协议号
                alt 对不上或已经取走
                    Rpc->>Rpc: 丢掉，不唤醒
                else 对上
                    Wait->>Wait: FinishSlot 里 DecodeReply
                    Note over Wait: 先读 int32。只有 0 才写进等待器私有对象；解不开改成 OI_ERR_FAIL
                    Wait->>Worker: 已经挂起才 Schedule
                    Note over Worker: 工人稍后 resume。await_resume 只在码为 0 时交出这份对象
                end
            end
            end
        end
    end
    alt 数据报
        Sess->>Sess: 清掉剩余字节，不拼到下一包
    else TCP 已经关闭
        Sess->>Sess: 取消计时并清空
    else TCP 还有残包
        Sess->>Sess: 挂一次性计时，已有计时不延期
    else 缓冲已空
        Sess->>Sess: 取消计时
    end
    opt 这次因顶到额度停下，且没关、读事件还在、额度仍是 0
        Sess->>Sess: PassiveClose(ENOBUFS)
    end
```

计时用 `incomplete_frame_timeout_ms`，默认 10000，到点 `PassiveClose(ETIMEDOUT)`；计时器没配上则是 `ENOBUFS`。对端关闭或读出错时，只要有错误码或读事件还在，也会 `PassiveClose`。回包比挂起先到时完成门已是 Ready，不再 `Schedule`。
