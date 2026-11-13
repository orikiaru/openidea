#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class BackpressureMgr
{
	DECLARE_SINGLETON(BackpressureMgr)
	BackpressureMgr() = default;
	// 建连前排队、已连接直发、固定 sid 超出发送额度都回 BUSY。关闭后固定 sid 回 CONNECT。
	OI::Coro::TaskResult<int> CheckDirectBackpressure();

  public:
	// 建连前排队、已连接直发、固定 sid 超出发送额度都回 BUSY。小请求仍成功，关闭后固定 sid 回 CONNECT。
	OI::Coro::TaskResult<int> Run();
};
