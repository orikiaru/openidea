# 单向消息

普通 `Notify` 使用调用号 0，不登记等待，不回包。`ProtoMultiClientManager` 的 `NotifyPeer` 和 `NotifyAny` 则复用相同协议号，启动异步 RPC 任务并等待回执，默认超时 180 秒。任务持有消息副本；返回 `OI_OK` 只表示任务已启动，不代表对端收到，发送、回执或超时失败由任务错误处理器报告。

## 接口

| 用途 | 声明 |
| --- | --- |
| 管理器按 sid 发送 | [`ProtoSessionManager::Notify`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L76) |
| 发回这次请求的会话 | [`RpcContext::Notify`](../../cbaselib/proto/inc/oi_rpc_coro.h#L52) |
| 用这次请求的管理器发给另一条会话 | [`RpcContext::Notify`](../../cbaselib/proto/inc/oi_rpc_coro.h#L58) |
| UDP 地址换成 sid | [`OpenPeer`](../../cbaselib/net/inc/oi_session_manager.h#L155) |
| 客户端当前会话 | [`ConnectedSid`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L140) |
| 从形参推出，含成员函数 | [`RegisterNotifyHandler`](../../cbaselib/proto/inc/oi_rpc_coro.h#L368) |
| 显式消息类型 | [`RegisterNotifyHandler`](../../cbaselib/proto/inc/oi_rpc_coro.h#L350) |
| 声明处理函数 | [`OI_NOTIFY_HANDLER`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1470) |

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

返回 [`SendBytes`](../../cbaselib/net/inc/oi_session_manager.h#L190) 的错误码。编码失败是 `OI_ERR_ENCODE`。会话不在是 `OI_ERR_CONNECT`。

在接收这条连接的管理器上登记。自己的 `Init` 里写 `RegisterNotifyHandler(fn)` 或 `RegisterNotifyHandler(&类::成员)`；在别处写 `管理器.RegisterNotifyHandler(fn)`，例如 `IpcUdp::GetInstance().RegisterNotifyHandler(...)`。`RegisterNotifyHandler(fn)` 从形参推出消息类型，显式写 `RegisterNotifyHandler<Msg>(fn)`。成员函数的类检查与 `RegisterRpcHandler` 相同：当前管理器必须是该类或其派生，否则抛 `std::logic_error`。

```cpp
::OI::Rpc::RpcTask IpcUdp::HandleNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest)
{
    co_return stContext.Notify(stRequest);
}

int32_t IpcUdp::Init()
{
    RegisterNotifyHandler(&IpcUdp::HandleNote);
    return ::OI::Rpc::ProtoSessionManager::Init();
}
```

> **注意：** 一个协议号在同一个管理器上只登记一次：要么 `RegisterRpcHandler`，要么 `RegisterNotifyHandler`。重复登记记日志并抛 `std::logic_error`，不覆盖，模块初始化失败退出。别的管理器有自己的表。

> **注意：** RPC 包装通知的接收端需使用带 RPC 适配的新版 `RegisterNotifyHandler`；旧版接收端不会为其回包，发送任务会等待到超时。`RegisterNotifyHandler` 复用 RPC 处理路径。调用号为 0 时不回包；调用号非 0 时回传处理返回码，成功时附带默认构造的关联响应。

> **注意：** 服务端执行额度满时，调用号为 0 的消息直接丢掉；带调用号的 RPC 包装通知按 RPC 路径返回 `OI_ERR_BUSY`。

> **注意：** UDP 上 `sendto` 没接住整包时，`RpcNotify` 返回 `OI_ERR_FAIL`，这一包已经从队列丢掉，不会自动重发。
