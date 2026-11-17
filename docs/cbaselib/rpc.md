# 请求回包

一条 rpc 有请求和回复，共用一个协议号。0 成功，负数是框架错误，正数是业务错误。细节见 [协程](../coroutine.md) 的服务端和客户端。

## 接口

| 用途 | 声明 |
| --- | --- |
| 从形参推出，含成员函数 | [`HandleRpc`](../../cbaselib/proto/inc/oi_rpc_coro.h#L312) |
| 显式请求类型 | [`HandleRpc`](../../cbaselib/proto/inc/oi_rpc_coro.h#L305) |
| 显式请求和回包 | [`HandleRpc`](../../cbaselib/proto/inc/oi_rpc_coro.h#L299) |
| 协程处理函数 | [`OI_RPC_HANDLER`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1460) |
| 同步处理函数 | [`OI_RPC_SYNC_HANDLER`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1462) |
| 带会话上下文 | [`OI_RPC_CONTEXT_HANDLER`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1464) |
| 从上下文拿回组件 | [`OI_RPC_SELF`](../../cbaselib/proto/inc/oi_rpc_coro.h#L1467) |
| 指定会话调用，结果写入输出 | [`RpcCall`](../../cbaselib/proto/inc/oi_rpc_coro.h#L720) |
| 客户端管理器上调用 | [`RpcCall`](../../cbaselib/proto/inc/oi_rpc_coro.h#L808) |
| 不限分区 | [`CallAny`](../../cbaselib/proto/inc/oi_rpc_session_manager.h#L166) |
| 带响应体的结果 | [`RpcResult`](../../cbaselib/proto/inc/oi_rpc_coro.h#L639) |

```cpp
::OI::Rpc::RpcTask RoomServer::HandleJoin(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::JoinReq& stRequest, ::OI::RpcData::JoinRes& stResponse)
{
	co_return co_await RoomMgr::GetInstance().Join(stContext, stRequest, stResponse);
}

int32_t RoomServer::Init()
{
	HandleRpc(&RoomServer::HandleJoin);
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

> **注意：** 注册是会话管理器的成员。在自己的 `Init` 里写 `HandleRpc(fn)` 或 `HandleRpc(&类::成员)`；在别处写 `管理器.HandleRpc(fn)`。请求处理表是 `ProtoSessionManager` 的私有锁和 map，按管理器各存一份，登记 `BindRpcHandler`，查找 `FindRpcHandler`（实现在 `oi_rpc_session_manager.cpp`）。只有登记到接收该连接的那个管理器上才生效。同一管理器重复登记同一协议号返回 `OI_ERR_EXIST` 并记日志，不覆盖。

> **注意：** `HandleRpc(fn)` 从形参推出类型。显式写 `HandleRpc<Req>(fn)` 或 `HandleRpc<Req, Res>(fn)`，这两个重载收自由函数或 lambda。成员函数在登记时把当前管理器 `dynamic_cast` 成所属类：当前对象必须是该类或其派生，否则返回 `OI_ERR_INVALID_ARGUMENT`。该类还须继承 `ProtoSessionManager`。函数体里拿组件用 `OI_RPC_SELF(Type)`，它转换 `stContext.stManager`，对象要活过所有在途请求。

> **注意：** 输出引用版 `RpcCall` 先看返回码，再读输出。非 0 时输出保持调用前的值。时长用 [`OI::Timer::MS`](../../cbaselib/timer/inc/oi_duration.h#L65)，默认 [`Seconds(180)`](../../cbaselib/proto/inc/oi_rpc_coro.h#L722)。

> **注意：** 不带输出的直连等待对象只能 `co_await` 一次。再次等待或等待已经移走的对象会抛 `std::logic_error`。

> **注意：** 服务端额度满返回 `OI_ERR_BUSY`，不创建业务协程。框架不自动重放。`Rpc.max_inflight_count` 等仍在第一次用到时读取，非法配置让 `ProtoSessionManager::Init` 返回 `OI_ERR_INVALID_ARGUMENT`。
