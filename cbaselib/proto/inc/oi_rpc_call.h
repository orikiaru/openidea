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
	// 这条调用所属的会话管理器。
	::OI::Net::SessionManager* pManager = nullptr;
	// fnSubmit / fnDrop 的第一个参数，一般是管理器或路由自己。
	void* pContext = nullptr;
	// 连接就绪后把调用号和包体发出去。
	void (*fnSubmit)(void*, uint64_t, const ::OI::Base::Bytes&) = nullptr;
	// 调用取消或失败时丢掉还没发出的那一份。
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
	// 还在等回包的调用数。
	size_t iCalls = 0;
	// 挂在超时链上的节点数。
	size_t iExpirations = 0;
	// 超时分桶里非空的桶数。
	size_t iBuckets = 0;
	// 按会话索引的调用数。
	size_t iSessionEntries = 0;
	// 有在途调用的会话数。
	size_t iSessionBuckets = 0;
};
// 各分片在途调用的合计。并发请求下不是同一时刻的快照。
RpcWaitStats GetRpcWaitStats();
// 读取进程级 RPC 在途限制；非法配置令模块初始化失败。
bool InitRpcAdmission();

// 连接还没好，这条调用先别发。调用号已经不在表里返回 false。
bool MarkRpcAwaitingConnect(uint64_t iCallId);
// 连接好了，把调用记到这个会话上，并清掉等待连接标记。调用号不在返回 false。
bool ClaimRpcSend(uint64_t iCallId, sid_t iSessionId);
// 回包按调用号交给 RpcCall 登记的等待项，请求交给该协议号的处理协程。都没有就把包丢掉。
bool DeliverRpc(proto_type_t iId, ::OI::Base::BytesStream& stBody, ::OI::Net::SessionManager* pManager, sid_t iSessionId);

}
}
