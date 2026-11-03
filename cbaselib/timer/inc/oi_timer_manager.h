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
#include <type_traits>
#include <utility>
#include <memory>
#include <chrono>
namespace OI { namespace Timer {

typedef std::deque<TimerLevel> TimerLevelVector;

// 到点后在时间轮工作线程放锁执行。lambda 是 ()，要用的数据自己捕获。
template<typename F>
class FnTimer : public BaseTimer
{
	F m_fnFunction;
public:
	explicit FnTimer(F fnFunctionParam) : m_fnFunction(std::move(fnFunctionParam)) {}
	bool OnTimer()
	{
		// Update 已在有序工作线程上，且已释放时间轮锁；直接执行，不能受普通任务限额影响。
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
	bool Init();
	// libuv 定时器到点。只把 Update 丢进任务队列，不在 poll 回调里跑时间轮
	void OnSignal();
	// 定时器实际触发。单线程在主循环取到任务后执行，多线程在有序队列线程执行
	void Update();

	timer_tick_t GetCurTick()				{	return m_iCurrentTick;	}
	const struct timeval& GetCurTickTime()	{	return m_stCurrentTimeValue;		}
	time_t		GetTime()					{	return m_iCurrentTime; }
	const tm&	GetTm()						{	return m_stCurrentCalendarTime; }
	int			GetTickOffsetMS()			{	return m_iTickOffsetMilliseconds; }

	uint32_t milling2tick(int iMillisecondParam)
	{
		int iOffset = iMillisecondParam / m_iTickOffsetMilliseconds;
		//定时器时间向tick时间修正取整
		iOffset += iMillisecondParam % m_iTickOffsetMilliseconds > 0 ? 1 : 0;
		return iOffset;
	}

	void AddTimer(BaseTimer* pTimerParam, int iMillisecondParam);

	template<typename F>
	typename std::enable_if<!std::is_base_of<BaseTimer, typename std::remove_pointer<typename std::decay<F>::type>::type>::value, void>::type
	AddTimer(F fnFunctionParam, int iMillisecondParam)
	{
		typedef typename std::decay<F>::type Fn;
		AddTimer(new FnTimer<Fn>(std::move(fnFunctionParam)), iMillisecondParam);
	}

	bool RemoveTimer(BaseTimer* pTimerParam)
	{
		::OI::Thread::MutexGuard stLock(m_stWheelMutex);
		assert(pTimerParam->GetTimerLevel());
		return pTimerParam->GetTimerLevel()->RemoveTimer(this, pTimerParam);
	}
	std::string GenTraceLog();
private:
	// 相对延时只用单调时钟；系统校时只影响日历快照。
	int64_t TicksFromNow(int iMillisecondParam) const;

	//增加定时器
	void add_timer(BaseTimer* pTimerParam);
	//执行定时器。不持锁调用，续期时自己加锁挂回去
	void run_timer(BaseTimer* pTimerParam);
	//推进 1 tick，本 tick 到点的放进 due。持 _wheel_mu 调用
	void run_tick(TimerList& vDueParam);
	//均衡高级定时器
	void try_uniform();
	//取出最低一级本 tick 到点的定时器
	void try_run_timer(TimerList& vDueParam);

private:
	friend class TimerLevel;
	::OI::Thread::Mutex m_stWheelMutex;

	// 最后一个已执行 tick 的单调时钟基点，未满一跳的余数留到下一次。
	std::chrono::steady_clock::time_point m_stLastTickMonotonic;

	// 最近一次 Update 的日历时间，独立于相对定时。
	struct timeval		m_stCurrentTimeValue;

	//当前tick数
	timer_tick_t		m_iCurrentTick;
	//每tick的时间间隔
	uint32_t			m_iTickOffsetMilliseconds;
	//timer层级数
	size_t				m_iTimerLevelCount;
	//当前timer层级数组,0代表与tick同步的定时器列表
	TimerLevelVector	m_vTimerLevels;		//分级的timer

	time_t				m_iCurrentTime;
	struct tm			m_stCurrentCalendarTime;
};
}}
