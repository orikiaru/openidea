# 单向消息

## 发出

按 sid 的 `Notify` 和 `RpcContext::Notify` 走 `RpcNotify`。多线程里 `CheckThreadAccess` 直接返回 `OI_OK`，不因调用线程拒绝。包体调用号写 0，不登记等待，当场 `SendFrame`。返回值是编码或发送的错误码。打开写事件是 `PostOp`，`send` 在 poll 线程。

`ProtoClientManager::Notify`、多连接的四个 `Notify`，以及 `RouteManager` 的四个 `Notify`，走 `StartTask`。返回值只表示任务已经启动。消息按值拷进闭包。任务里再 `RpcCall`：`Bind` 分配调用号，包体是调用号或上请求标记，登记等待，默认 180 秒。发送、回执或超时失败在任务里抛出，独立任务交给 `ReportTaskError`，不改调用方已经返回的值。

`StartTask` 进工人队列，不在 poll 回调里 `resume`。收发字节仍在 poll 线程。帧进发送缓冲之后同 [发出一次调用](send.md)。

`ProtoClientManager::SubmitCall` 只在还没连过时 `HoldCall`。已经连过再断线，`Wake` 成 `OI_ERR_CONNECT`，不暂存。`PeerConnection::Submit` 不看连过没有：当前没有会话就放进 `vHeld`，`Ensure` 去连，这次连上由 `Finish` 补发。路由的发送目标也是 `PeerConnection`。

```mermaid
sequenceDiagram
    autonumber
    participant Worker as 工人
    participant Caller as 调用方
    participant Task as 通知任务
    participant Route as RouteAwaiter
    participant Call as RpcCall
    participant Wait as 等待表
    participant Peer as 发送目标
    participant Poll as poll 线程

    alt RpcNotify，按 sid 或 RpcContext
        Caller->>Caller: 包体调用号写 0，不登记等待
        alt 编码失败
            Caller-->>Caller: 返回 OI_ERR_ENCODE
        else SendFrame 失败
            Caller-->>Caller: 会话不在是 OI_ERR_CONNECT，其它错误码原样返回
        else 写入发送缓冲
            Caller->>Poll: AllowSend 变成 PostOp
            Caller-->>Caller: 返回 OI_OK
            Poll->>Poll: DataOut 里 send
        end
    else RouteManager::Notify 分区号为负
        Caller-->>Caller: 返回 OI_ERR_FAIL，不启动
    else 单个发送目标是空
        Caller-->>Caller: 返回 OI_ERR_CONNECT，不启动
    else 启动任务
        Caller->>Worker: StartTask 入工人队列
        alt RouteManager 的四个 Notify
            Caller-->>Caller: 返回 OI_OK
        else NotifyAll 或 NotifyMany
            Caller-->>Caller: 空目标跳过，返回启动条数
        else ProtoClient、NotifyPeer、NotifyAny
            Caller-->>Caller: 返回 OI_OK
        end
        Worker->>Task: resume
        alt 按服务名，选路失败
            Task->>Route: co_await，截止 180 秒
            Task->>Task: 非 0 结果抛出，ReportTaskError
        else 发出 RpcCall
            alt 按服务名
                Task->>Route: co_await，截止 180 秒
                Task->>Call: 每个目标一份，先 Submit 再逐个等
            else 已经有发送目标
                Task->>Call: RpcCall，超时 180 秒
            end
            Call->>Wait: Bind 分配调用号并放入分片
            Call->>Call: 包体 = 调用号或上请求标记，再加消息
            Call->>Peer: Submit
            alt ProtoClient 连过又断了，或目标已退役
                Peer->>Wait: 当场 Wake OI_ERR_CONNECT，不暂存
                Task->>Task: 不挂起，抛出，ReportTaskError
            else 没连上且暂存超限
                Peer->>Wait: 当场 Wake OI_ERR_BUSY
                Task->>Task: 不挂起，抛出，ReportTaskError
            else ProtoClient 还没连过，或 Peer、路由没连上
                Peer->>Peer: HoldCall 或 vHeld，然后去连
                alt 这次连上
                    Poll->>Peer: FlushHeldCalls 或 Finish 补发
                    alt 回执是 0
                        Wait->>Worker: Schedule
                        Worker->>Task: resume，任务结束
                    else 补发失败、回执非 0 或超时
                        Wait->>Worker: Schedule
                        Worker->>Task: resume 后抛出，ReportTaskError
                    end
                else 建连失败，或还在暂存里超时
                    Poll->>Wait: Wake OI_ERR_CONNECT，不留给下次重试
                    Wait->>Worker: Schedule
                    Worker->>Task: resume 后抛出，ReportTaskError
                end
            else 当前有会话
                Peer->>Poll: ClaimSend 后 SendFrame，AllowSend 是 PostOp
                alt 发送当场失败
                    Peer->>Wait: Wake
                    Task->>Task: 不挂起，抛出，ReportTaskError
                else 回执是 0
                    Wait->>Worker: Schedule
                    Worker->>Task: resume，任务结束
                else 回执非 0 或超时
                    Wait->>Worker: Schedule
                    Worker->>Task: resume 后抛出，ReportTaskError
                end
            end
        end
    end
```

负分区和空目标仍同步返回，不启动。`NotifyAll` / `NotifyMany` 可以启动 0 条。服务或分区不存在留在路由任务里，`RouteCall::Error` 的 default 是 `OI_ERR_CONNECT`。还在暂存里到期写成 `OI_ERR_CONNECT`，发出之后到期写成 `OI_ERR_TIMEOUT`，见 [调用超时](timeout.md)。这次建连失败会把 `vHeld` 唤醒成 `OI_ERR_CONNECT`，不留给下一次重试。UDP 上 `sendto` 失败发生在入队之后。`RpcNotify` 当时已经按入队结果返回，通常是 `OI_OK`。这一包被丢掉，已经登记的调用不会因此结束。

## 收到

切帧同 [收到一帧](receive.md)。这里只看调用号 0。读和解开在 poll 线程。

`Deliver` 读出调用号。0 是 `bIsNotify`，按请求找 `FindRpcHandler`。没有处理函数就丢。`AcquireAdmission` 拿不到租约也丢，不回 `OI_ERR_BUSY`。解开才 `StartTask`，进工人队列。处理协程在工人上。`Run` 看见原始调用号 0，业务结束后直接 `co_return`，不编码回包。这条路径没有回包要 `send`。解不开不回包，不启动协程。

```mermaid
sequenceDiagram
    autonumber
    participant Poll as poll 线程
    participant Sess as ProtoSession
    participant Rpc as RpcCallManager
    participant Make as RpcHandlerBuilder
    participant Worker as 工人
    participant Run as Run

    Sess->>Rpc: Deliver 协议号和包体
    Rpc->>Rpc: 读出调用号 0，bIsNotify
    alt 读不出调用号
        Rpc->>Rpc: 丢掉
    else 没有处理函数
        Rpc->>Rpc: FindRpcHandler 为空，丢掉
    else 入口额度不足
        Rpc->>Rpc: 丢掉，不回包
    else 有额度
        Rpc->>Make: poll 线程解开请求
        alt 解不开
            Make->>Make: 原始调用号是 0，不回包，不启动
        else 解开
            Make->>Worker: StartTask 入队
            Worker->>Run: resume
            alt 业务当场结束
                Run->>Run: bNotify，直接 co_return，不编码回包
            else 业务自己挂起
                Run-->>Worker: 停住，工人去干别的
                Note over Worker: 结束时 PostCoroutine，再回到 Run，仍不写回包
            end
        end
    end
```

业务返回码不写回包。处理函数抛出只报告，也不回包。
