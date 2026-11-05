#pragma once

#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include <functional>
namespace OI
{
namespace Rpc
{

// 请求的提交目标。应用客户端和路由连接共用发送/撤销逻辑，连接不再冒充管理器。
struct RpcSender
{
	::OI::Net::SessionManager* pManager = nullptr;
	void* pContext = nullptr;
	void (*fnSubmit)(void*, uint64_t, const ::OI::Base::Bytes&) = nullptr;
	void (*fnDrop)(void*, uint64_t) = nullptr;
	::OI::Net::SessionManager& Manager() const
	{
		return *pManager;
	}
	void SubmitCall(uint64_t iCallId, const ::OI::Base::Bytes& stData)
	{
		fnSubmit(pContext, iCallId, stData);
	}
	void DropPendingCall(uint64_t iCallId)
	{
		fnDrop(pContext, iCallId);
	}
};

// 分片逐个取快照，用于诊断；有并发请求时不保证全局同一时刻的一致性。
struct RpcWaitStats
{
	size_t iCalls = 0;
	size_t iExpirations = 0;
	size_t iBuckets = 0;
	size_t iSessionEntries = 0;
	size_t iSessionBuckets = 0;
};
RpcWaitStats GetRpcWaitStats();
// 读取进程级 RPC 在途限制；非法配置令模块初始化失败。
bool InitRpcAdmission();

bool MarkRpcAwaitingConnect(uint64_t iCallId);
bool ClaimRpcSend(uint64_t iCallId, sid_t iSessionId);
// 回包按调用号交给 RpcCall 登记的等待项，请求交给该协议号的处理协程。都没有就把包丢掉。
bool DeliverRpc(proto_type_t iId, ::OI::Base::BytesStream& stBody, ::OI::Net::SessionManager* pManager, sid_t iSessionId);

}
}
