# 请求回包

一条 rpc 有请求和回复，共用一个协议号。0 成功，负数是框架错误，正数是业务错误。细节见 [协程](../coroutine.md) 的服务端和客户端。

## 接口

| 用途 | 声明 |
| --- | --- |
| 注册处理函数 | [`HandleRpc`](../../cbaselib/proto/inc/oi_rpc_coro.h#L181) |
| 成员函数注册 | [`OI_RPC_BIND_SELF`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1165) |
| 协程处理函数 | [`OI_RPC_HANDLER`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1154) |
| 带会话上下文 | [`OI_RPC_CONTEXT_HANDLER`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1158) |
| 从上下文拿回组件 | [`OI_RPC_SELF`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1163) |
| 指定会话调用，结果写入输出 | [`RpcCall`](../../cbaselib/proto/inc/oi_rpc_coro.h#L548) |
| 客户端管理器上调用 | [`RpcCall`](../../cbaselib/proto/inc/oi_rpc_coro.h#L636) |
| 不限分区 | [`CallAny`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L208) |
| 带响应体的结果 | [`RpcResult`](../../cbaselib/proto/inc/oi_rpc_coro.h#L467) |

```cpp
::OI::Rpc::RpcTask RoomServer::HandleJoin(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::JoinReq& stRequest, ::OI::RpcData::JoinRes& stResponse)
{
	co_return co_await RoomMgr::GetInstance().Join(stContext, stRequest, stResponse);
}

bool RoomServer::Init()
{
	OI_RPC_BIND_SELF(Join, RoomServer, HandleJoin);
	return ::OI::Rpc::ProtoSessionManager::Init();
}
```

```cpp
const int32_t iRet = co_await ::OI::Rpc::RpcCall(RoomClient::GetInstance(), stRequest, stResponse, ::OI::Timer::Seconds(3));
if (iRet != OI_OK)
{
	co_return iRet;
}
```

> **注意：** 处理函数 `co_return` 错误码。回包不再另带一个 `iCode`。失败时框架不序列化回复体。

> **注意：** `OI_RPC_BIND_SELF` 不在注册时绑 `this`。处理时从这次回调的 `stContext.stManager` 还原对象。对象要活过所有在途请求。

> **注意：** 输出引用版 `RpcCall` 先看返回码，再读输出。非 0 时输出保持调用前的值。时长用 [`OI::Timer::MS`](../../cbaselib/timer/inc/oi_duration.h#L65)，默认 [`Seconds(180)`](../../cbaselib/proto/inc/oi_rpc_coro.h#L550)。

> **注意：** 不带输出的直连等待对象只能 `co_await` 一次。再次等待或等待已经移走的对象会抛 `std::logic_error`。

> **注意：** 服务端额度满返回 `OI_ERR_BUSY`，不创建业务协程。框架不自动重放。
