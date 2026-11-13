#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class DeferredRpcProbe;

class RpcAdmissionMgr
{
	DECLARE_SINGLETON(RpcAdmissionMgr)
	RpcAdmissionMgr() = default;
	// 发一次 Hold。成功且标记对上返回 0，其他结果原样返回状态码。
	OI::Coro::TaskResult<int32_t> AdmissionCall(DeferredRpcProbe* pClient);
	// 服务端在途条数或字节超限时回 BUSY，额度恢复后请求仍能完成。
	OI::Coro::TaskResult<int> CheckRpcAdmission();

  public:
	// 服务端在途条数或字节超限时回 BUSY，额度恢复后请求仍能完成。只在准入用例里跑。
	OI::Coro::TaskResult<int> Run();
};
