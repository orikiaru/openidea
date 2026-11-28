# 单向消息

普通 `Notify` 使用调用号 0，不登记等待，不回包。`ProtoClientManager::Notify`、`ProtoMultiClientManager` 的 `NotifyPeer` / `NotifyAny` / `NotifyAll` / `NotifyMany`，以及 `RouteManager` 的四个 `Notify`，复用同一协议号，复制消息并启动异步 RPC 任务，等待回执，默认超时 180 秒。返回 `OI_OK` 或启动条数只表示任务已启动，不代表对端收到。发送、回执或超时失败由 `OI::Coro::ReportTaskError` 交给 `OI::Coro::SetTaskErrorHandler`。线程不对时这些接口同步返回错误码，不启动任务。

`ProtoClientManager::Notify` 只在第一次连上前暂存。已经连过再断线，`SubmitCall` 得到 `OI_ERR_CONNECT` 后直接失败，不暂存、不补发。`ProtoMultiClientManager` 和 `RouteManager` 走 `PeerConnection`：当前没连上，包括断线之后，还没发出的通知先暂存，重连成功再补发。

`ProtoMultiClientManager::NotifyAll` 按这一刻的名单给每台各启动一条，`NotifyMany` 只发指定实例，名单里没有的跳过；两者返回启动了几条。`RouteManager` 的四个 `Notify` 返回 `OI_OK`。`Notify(名字, 分区号, msg)` 在分区号为负时同步返回 `OI_ERR_FAIL`，不启动任务。不存在的服务或分区由协程任务错误处理器报告，路由失败映射为 `OI_ERR_CONNECT`（`RouteCall::Error` 的 default）。

时序见 [单线程](../sequence-diagram/single-thread/notify.md) 和 [多线程](../sequence-diagram/multi-thread/notify.md)。

## 接口

| 用途 | 声明 |
| --- | --- |
| 管理器按 sid 发送 | [`ProtoSessionManager::Notify`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L176) |
| 直连这一条连接 | [`ProtoClientManager::Notify`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L278) |
| 多连接指定实例 | [`ProtoMultiClientManager::NotifyPeer`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L375) |
| 多连接任选一台 | [`ProtoMultiClientManager::NotifyAny`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L386) |
| 多连接每一台 | [`ProtoMultiClientManager::NotifyAll`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L397) |
| 多连接指定若干实例 | [`ProtoMultiClientManager::NotifyMany`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L411) |
| 按服务名任选 | [`RouteManager::Notify`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L534) |
| 按服务名和分区 | [`RouteManager::Notify`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L540) |
| 按服务名每一台 | [`RouteManager::NotifyAll`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L549) |
| 按服务名若干分区 | [`RouteManager::NotifyMany`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L555) |
| 发回这次请求的会话 | [`RpcContext::Notify`](../../cbaselib/proto/inc/oi_rpc_coro.h#L55) |
| 用这次请求的管理器发给另一条会话 | [`RpcContext::Notify`](../../cbaselib/proto/inc/oi_rpc_coro.h#L61) |
| UDP 地址换成 sid | [`OpenPeer`](../../cbaselib/net/inc/oi_session_manager.h#L139) |
| 客户端当前会话 | [`ConnectedSid`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L241) |
| 从形参推出，含成员函数 | [`RegisterNotifyHandler`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L101) |
| 显式消息类型 | [`RegisterNotifyHandler`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L138) |
```cpp
stContext.Notify(stReply);

int32_t iCode = stManager.Notify(iSessionId, stFrame);
sid_t iUdpSessionId = INVALID_SID;
int32_t iUdp = stUdp.OpenPeer(sIp, sPort, iUdpSessionId);
if (iUdp == OI_OK)
{
    iUdp = stUdp.Notify(iUdpSessionId, stFrame);
}
```

按 sid 的 `Notify` 和 `RpcContext::Notify` 返回 [`SendBytes`](../../cbaselib/net/inc/oi_session_manager.h#L171) 的错误码。编码失败是 `OI_ERR_ENCODE`。会话不在是 `OI_ERR_CONNECT`。

需要等待原连接上的回复时，使用 RPC 协议管理器的 `Call(sid, 请求, 回复, 时长)`；多实例管理器按实例号调用的重载不接受 sid，两个标识不可混用。

在接收这条连接的管理器上登记。仓库随附业务的 notify 处理函数返回 `OI::Rpc::RpcTask`，首参为 `const OI::Rpc::RpcContext&`，后接请求；框架手写接入仍支持同步处理函数或省略 Context。模块协议声明可生成注册代码；手写登记时，自己的 `Init` 里写 `RegisterNotifyHandler(fn)` 或 `RegisterNotifyHandler(&类::成员)`，在别处写 `管理器.RegisterNotifyHandler(fn)`。`RegisterNotifyHandler(fn)` 从形参推出消息类型，显式写 `RegisterNotifyHandler<Msg>(fn)`。成员函数的类检查与 `RegisterRpcHandler` 相同：当前管理器必须是该类或其派生，否则抛 `std::logic_error`。

```cpp
::OI::Rpc::RpcTask IpcUdp::HandleUdpNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest)
{
    co_return stContext.Notify(stRequest);
}

```

`IpcUdp` 在模块声明中写 `notify UdpNote;`，生成器负责登记，不再手动重复登记。

> **注意：** 一个协议号在同一个管理器上只登记一次：要么 `RegisterRpcHandler`，要么 `RegisterNotifyHandler`。重复登记记日志并抛 `std::logic_error`，不覆盖，模块初始化失败退出。别的管理器有自己的表。

> **注意：** RPC 包装通知的接收端需使用带 RPC 适配的新版 `RegisterNotifyHandler`；旧版接收端不会为其回包，发送任务会等待到超时。`RegisterNotifyHandler` 复用 RPC 处理路径。调用号为 0 时不回包；调用号非 0 时回传处理返回码，成功时附带默认构造的关联响应。

> **注意：** 服务端执行额度满时，调用号为 0 的消息直接丢掉；带调用号的 RPC 包装通知按 RPC 路径返回 `OI_ERR_BUSY`。

> **注意：** UDP 上 `sendto` 没接住整包时，`RpcNotify` 返回 `OI_ERR_FAIL`，这一包已经从队列丢掉，不会自动重发。
