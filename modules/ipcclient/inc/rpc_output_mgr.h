#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class RpcOutputMgr
{
	DECLARE_SINGLETON(RpcOutputMgr)
	RpcOutputMgr() = default;
	// 16 个等待者只提交一次。重复等待、固定会话、业务错误和迟到回包都不覆盖输出。
	OI::Coro::TaskResult<int> CheckOutputRpcTask(sid_t iSessionId);
	// 按值和输出引用同时在飞。没连上时，固定 sid 回 CONNECT。
	OI::Coro::TaskResult<int> CheckDeferredRpcSession();

  public:
	// 按值和输出引用同时在飞。没连上时，固定 sid 回 CONNECT。
	OI::Coro::TaskResult<int> RunDeferred();
	// 16 个等待者只提交一次，重复等待不重写输出。固定会话、业务错误、超时后的迟到回包都不覆盖新结果。
	OI::Coro::TaskResult<int> Run(sid_t iSessionId);
};
