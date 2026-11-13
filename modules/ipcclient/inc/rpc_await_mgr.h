#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class RpcAwaitMgr
{
	DECLARE_SINGLETON(RpcAwaitMgr)
	RpcAwaitMgr() = default;
	// 等这一次调用。已经完成后再等会拒绝。
	template <typename T>
	OI::Coro::TaskResult<bool> AwaitSingleUse(std::shared_ptr<T> pCall);
	// 两次并发等待只能接受一次，完成后再等也必须拒绝。
	template <typename T>
	OI::Coro::TaskResult<int> CheckSingleUse(std::shared_ptr<T> pCall);
	// 等同一次建连。创建时就跑到挂起。
	OI::Coro::TaskResult<bool> AwaitConnect(std::shared_ptr<OI::Net::SessionManager::ConnectAwaiter> pConnect);
	// RPC、HTTP 和 32 个协程的建连都只能成功等待一次。移走后再等会拒绝。
	OI::Coro::TaskResult<int> CheckAwaitOwnership();

  public:
	// 单次等待器并发等只能成功一次。32 个协程共享同一次建连，移走后再等会拒绝。
	OI::Coro::TaskResult<int> Run();
};
