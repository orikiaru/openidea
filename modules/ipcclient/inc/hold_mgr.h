#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <atomic>
#include <cstdint>

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
	std::atomic<int32_t> m_iTickActive;
	std::atomic<int32_t> m_iTickOverlap;
	int64_t m_iWatchBeginTick;

  public:
	OI::Coro::Task OnTickUpdate(uint64_t iNowMilliseconds);
	void WatchUpdate(bool bWatch);
	int32_t TickUpdates() const
	{
		return m_iTickUpdates.load(std::memory_order_relaxed);
	}
	int32_t TickActive() const
	{
		return m_iTickActive.load(std::memory_order_relaxed);
	}
	int32_t TickOverlap() const
	{
		return m_iTickOverlap.load(std::memory_order_relaxed);
	}
	int64_t WatchBeginTick() const
	{
		return m_iWatchBeginTick;
	}
	void Load();
	bool Enabled() const
	{
		return m_bEnabled;
	}
	void Start(sid_t iSessionId);
	OI::Coro::Task Pump();
};
