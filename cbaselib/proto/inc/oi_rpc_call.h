#pragma once

#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include <functional>
namespace OI
{
namespace Rpc
{

// 分片逐个取快照，用于诊断；有并发请求时不保证全局同一时刻的一致性。
struct RpcWaitStats
{
	size_t iCalls = 0;
	size_t iExpirations = 0;
	size_t iBuckets = 0;
};
RpcWaitStats GetRpcWaitStats();
// 读取进程级 RPC 在途限制；非法配置令模块初始化失败。
bool InitRpcAdmission();

bool MarkRpcAwaitingConnect(uint64_t iCallIdParam);
bool ClaimRpcSend(uint64_t iCallIdParam, sid_t iSessionIdParam);
// 回包按调用号交给 RpcCall 登记的等待项，请求交给该协议号的处理协程。都没有就把包丢掉。
bool DeliverRpc(proto_type_t iIdParam, ::OI::Base::BytesStream& stBodyParam, ::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam);

}
}
