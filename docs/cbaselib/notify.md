# 单向消息

普通 `Notify` 使用调用号 0，不登记等待，不回包。`ProtoMultiClientManager` 的 `NotifyPeer` 和 `NotifyAny` 则复用相同协议号，启动异步 RPC 任务并等待回执，默认超时 180 秒。任务持有消息副本；返回 `OI_OK` 只表示任务已启动，不代表对端收到，发送、回执或超时失败由任务错误处理器报告。

## 接口

| 用途 | 声明 |
| --- | --- |
| 管理器按 sid 发送 | [`ProtoSessionManager::Notify`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L52) |
| 发回这次请求的会话 | [`RpcContext::Notify`](../../cbaselib/proto/inc/oi_rpc_coro.h#L51) |
| 用这次请求的管理器发给另一条会话 | [`RpcContext::Notify`](../../cbaselib/proto/inc/oi_rpc_coro.h#L57) |
| UDP 地址换成 sid | [`OpenPeer`](../../cbaselib/net/inc/oi_session_manager.h#L151) |
| 客户端当前会话 | [`ConnectedSid`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L127) |
| 注册处理函数 | [`HandleNotify`](../../cbaselib/proto/inc/oi_rpc_coro.h#L249) |
| 成员函数注册 | [`OI_NOTIFY_BIND_SELF`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1190) |

```cpp
stContext.Notify(stReply);

int32_t iCode = stManager.Notify(iSessionId, stFrame);
int32_t iUdp = stUdp.Notify(stUdp.OpenPeer(sIp, sPort), stFrame);
```

返回 [`SendBytesResult`](../../cbaselib/net/inc/oi_session_manager.h#L184) 的错误码。编码失败是 `OI_ERR_ENCODE`。会话不在是 `OI_ERR_CONNECT`。

> **注意：** 一个协议号只注册一种处理：要么 `HandleRpc`，要么 `HandleNotify`。

> **注意：** RPC 包装通知的接收端需使用带 RPC 适配的新版 `HandleNotify`；旧版接收端不会为其回包，发送任务会等待到超时。`HandleNotify` 复用 RPC 处理路径。调用号为 0 时不回包；调用号非 0 时回传处理返回码，成功时附带默认构造的关联响应。

> **注意：** 服务端执行额度满时，调用号为 0 的消息直接丢掉；带调用号的 RPC 包装通知按 RPC 路径返回 `OI_ERR_BUSY`。

> **注意：** UDP 上 `sendto` 没接住整包时，`RpcNotify` 返回 `OI_ERR_FAIL`，这一包已经从队列丢掉，不会自动重发。
