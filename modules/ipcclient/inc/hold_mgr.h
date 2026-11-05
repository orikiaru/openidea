#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

// 超时和回包撞车。和回声、日志、Redis 压测分开，关掉就不跑。
class HoldMgr
{
	DECLARE_SINGLETON(HoldMgr)
	HoldMgr();
	bool m_bEnabled;
	sid_t m_iSessionId;
	int32_t m_iRedisTimeoutMilliseconds;

  public:
	void Load();
	bool Enabled() const
	{
		return m_bEnabled;
	}
	void Start(sid_t iSessionId);
	OI::Coro::Task Pump();
};
