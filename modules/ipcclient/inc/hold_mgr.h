#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <atomic>
#include <ctime>

// 超时和回包撞车。和回声、日志、Redis 压测分开，关掉就不跑。
class HoldMgr
{
	DECLARE_SINGLETON(HoldMgr)
	HoldMgr();
	bool m_bEnabled;
	sid_t m_iSessionId;
	int32_t m_iRedisTimeoutMilliseconds;
	// 单例更新回归：打开后才计数。
	std::atomic<bool> m_bWatchUpdate;
	std::atomic<int32_t> m_iTickUpdates;
	std::atomic<int32_t> m_iSecondUpdates;

  public:
	void OnTickUpdate(uint64_t iNowMilliseconds);
	void OnSecondUpdate(time_t iNow);
	void WatchUpdate(bool bWatch);
	int32_t TickUpdates() const
	{
		return m_iTickUpdates.load(std::memory_order_relaxed);
	}
	int32_t SecondUpdates() const
	{
		return m_iSecondUpdates.load(std::memory_order_relaxed);
	}
	void Load();
	bool Enabled() const
	{
		return m_bEnabled;
	}
	void Start(sid_t iSessionId);
	OI::Coro::Task Pump();
};
