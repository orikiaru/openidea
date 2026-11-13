#pragma once
#include <sys/time.h>
#include <stdio.h>
#include <string.h>
#include "oi_header.h"
#include <deque>
#include <list>
#include <vector>
#include <time.h>
#include "oi_timer_level.h"
#include "oi_thread.h"
#include <assert.h>
#include <string>
#include "oi_singleton.h"
#include "oi_lock.h"
#include "oi_poll.h"
#include "oi_errno.h"
#include <type_traits>
#include <utility>
#include <memory>
#include <chrono>
namespace OI
{
namespace Timer
{

typedef std::deque<TimerLevel> TimerLevelVector;

// 到点后在 poll 线程放锁执行，不能阻塞或同步等待。lambda 是 ()，要用的数据自己捕获。
template <typename F>
class FnTimer : public BaseTimer
{
	F m_fnFunction;

  public:
	explicit FnTimer(F fnFunction)
	    : m_fnFunction(std::move(fnFunction))
	{
	}
	bool OnTimer()
	{
		// Update 已释放时间轮锁，直接执行。
		std::unique_ptr<FnTimer> pSelf(this);
		m_fnFunction();
		return END_TIMER;
	}
};

class BaseTimerManager
{
	DECLARE_SINGLETON(BaseTimerManager)
	BaseTimerManager();

  public:
	// 读 tick 间隔，记下单调时钟起点，并在 poll 上挂重复定时器。
	int32_t Init();
	// libuv 定时器到点，在 poll 线程直接调用 Update
	void OnSignal();
	// 定时器实际触发。只在 poll 线程执行，回调里不能同步等待网络或 Redis，耗时工作交给协程
	void Update();

	// 已经推进到的 tick。相对定时用它，不看系统时间。
	timer_tick_t GetCurTick()
	{
		return m_iCurrentTick;
	}
	// 最近一次 Update 的日历时间。
	const struct timeval& GetCurTickTime()
	{
		return m_stCurrentTimeValue;
	}
	// 最近一次 Update 的秒。
	time_t GetTime()
	{
		return m_iCurrentTime;
	}
	// 最近一次 Update 的本地日历。
	const tm& GetTm()
	{
		return m_stCurrentCalendarTime;
	}
	// 一跳多少毫秒，来自配置。
	int GetTickOffsetMS()
	{
		return m_iTickOffsetMilliseconds;
	}

	uint32_t milling2tick(int iMillisecond)
	{
		int iOffset = iMillisecond / m_iTickOffsetMilliseconds;
		//定时器时间向tick时间修正取整
		iOffset += iMillisecond % m_iTickOffsetMilliseconds > 0 ? 1 : 0;
		return iOffset;
	}

	// 按毫秒挂上一个定时器。对象所有权仍在调用方，除非 OnTimer 里自己销毁。
	int32_t AddTimer(BaseTimer* pTimer, int iMillisecond);
	template <typename F>
	typename std::enable_if<!std::is_base_of<BaseTimer, typename std::remove_pointer<typename std::decay<F>::type>::type>::value, int32_t>::type
	AddTimer(F fnFunction, int iMillisecond)
	{
		typedef typename std::decay<F>::type Fn;
		auto pTimer = std::make_unique<FnTimer<Fn>>(std::move(fnFunction));
		const int32_t iStatus = AddTimer(pTimer.get(), iMillisecond);
		if (iStatus == OI_OK)
		{
			pTimer.release();
		}
		return iStatus;
	}

	// 从时间轮摘掉。正在触发或未登记时返回 OI_ERR_NOT_FOUND。
	int32_t RemoveTimer(BaseTimer* pTimer)
	{
		return RemoveTimerResult(pTimer);
	}
	int32_t RemoveTimerResult(BaseTimer* pTimer)
	{
		const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("BaseTimerManager::RemoveTimer");
		if (iThreadStatus != OI_OK)
		{
			return iThreadStatus;
		}
		::OI::Thread::MutexGuard stLock(m_stWheelMutex);
		if (!pTimer || !pTimer->GetTimerLevel())
		{
			return OI_ERR_NOT_FOUND;
		}
		return pTimer->GetTimerLevel()->RemoveTimer(this, pTimer);
	}

	// 最近一次 Update 的单调时间和日历时间，给诊断用。
	std::string GenTraceLog();

  private:
	// 相对延时只用单调时钟；系统校时只影响日历快照。
	// 把毫秒换成相对上次 tick 的跳数，用单调时钟，不受系统校时影响。
	int64_t TicksFromNow(int iMillisecond) const;

	// 按目标 tick 挂进对应层。调用方已持有时间轮锁。
	void add_timer(BaseTimer* pTimer);
	// 锁外执行 OnTimer。要续期再加锁挂回去。
	void run_timer(BaseTimer* pTimer);
	// 推进 1 tick，这一跳到点的放进 vDue。调用方已持有时间轮锁。
	void run_tick(TimerList& vDue);
	// 把高层到期的定时器下放到更细的层。
	void try_uniform();
	// 取出最低层这一跳到点的定时器。
	void try_run_timer(TimerList& vDue);
	// 推进过 tick 后，为单例的 OnTickUpdate / OnSecondUpdate 各启动协程，不等它们结束。
	void DispatchUpdates();

  private:
	friend class TimerLevel;
	// 保护层级数组和 tick 游标。回调在锁外执行。
	::OI::Thread::Mutex m_stWheelMutex;

	// 最后一个已执行 tick 的单调时钟基点，未满一跳的余数留到下一次。
	std::chrono::steady_clock::time_point m_stLastTickMonotonic;

	// 最近一次 Update 的日历时间，独立于相对定时。
	struct timeval m_stCurrentTimeValue;

	//当前tick数
	timer_tick_t m_iCurrentTick;
	//每tick的时间间隔
	uint32_t m_iTickOffsetMilliseconds;
	//timer层级数
	size_t m_iTimerLevelCount;
	//当前timer层级数组,0代表与tick同步的定时器列表
	TimerLevelVector m_vTimerLevels; //分级的timer

	// 最近一次 Update 的秒。
	time_t m_iCurrentTime;
	// 已经分发给秒回调的秒。秒没变就不再调用。只在 Update 里读写。
	time_t m_iDispatchedSecond = 0;
	// 同一时刻的本地日历。
	struct tm m_stCurrentCalendarTime;
};
}
}
