#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class IsolationMgr
{
	DECLARE_SINGLETON(IsolationMgr)
	IsolationMgr() = default;
	// 重复 StartClient 之后，这一路 RPC 和 Redis 只收到自己的回包。
	OI::Coro::TaskResult<int> IsolationCall(int iIndex);
	// 32 路同时做上面这件事，已有连接不被顶掉。
	OI::Coro::TaskResult<int> CheckConnectionIsolation();

  public:
	// 重复 StartClient 不顶掉已有连接。RPC 和 Redis 各自只收到自己的回包。
	OI::Coro::TaskResult<int> Run();
};
