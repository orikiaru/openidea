#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <atomic>
#include <cstdint>

// 时间轮跳和定时器异常。秒更新在另一个单例上，一个单例不能两种都有。
class TimerMgr
{
	DECLARE_SINGLETON(TimerMgr)
	TimerMgr();
	std::atomic<bool> m_bWatchUpdate;
	std::atomic<int32_t> m_iTickUpdates;
	std::atomic<int32_t> m_iTickActive;
	std::atomic<int32_t> m_iTickOverlap;
	int64_t m_iWatchBeginTick;
	// 定时器回调抛异常后其余回调仍执行；跳更新挂起期间到点的跳跳过不补。
	OI::Coro::TaskResult<int> CheckTimerExceptions();

  public:
	// 时间轮跳回调。观察打开时记次数，上一次没结束就记重叠。
	OI::Coro::Task OnTickUpdate(uint64_t iNowMilliseconds);
	// 打开或关掉跳计数，并记下开始观察时的 tick。
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
	// 定时器回调抛异常后其余回调仍执行；跳更新和秒更新不重叠，挂起期间到点的跳跳过不补。
	OI::Coro::TaskResult<int> Run();
};
