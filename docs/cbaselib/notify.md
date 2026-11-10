# 单向消息

调用号 0，不登记等待，不回包。生成器仍要求写一个 `NameRes`，线上不会出现。

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

> **注意：** 处理函数的返回码只进日志，不会发回对端。

> **注意：** 服务端执行额度满时，单向消息直接丢掉，不回 `OI_ERR_BUSY`。

> **注意：** UDP 上 `sendto` 没接住整包时，`RpcNotify` 返回 `OI_ERR_FAIL`，这一包已经从队列丢掉，不会自动重发。
