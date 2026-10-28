#pragma once
#include <sys/time.h>
#include <string.h>
#include "header.h"
#include <deque>
#include <list>
#include <vector>
#include <time.h>
#include "base_timer_level.h"
#include "threadpool.h"
#include <assert.h>
#include <string>
#include "singleton.h"
#include "spin_lock.h"
#include <type_traits>
#include <utility>

typedef std::deque<TimerLevel> TimerLevelVector;

// 到点后把 lambda 拷贝进线程池。lambda 是 ()，要用的数据自己捕获。
template<typename F>
class FnTimer : public BaseTimer
{
	F _fn;
public:
	explicit FnTimer(F fn) : _fn(std::move(fn)) {}
	bool OnTimer()
	{
		F copied = _fn;
		// 跟时间轮同一条有序队列，紧跟这次 Update。不丢到普通队列里排队。
		OI::Thread::ThreadPool::GetInstance().AddTask(std::move(copied), OI::Thread::PRIO_QUEUE);
		delete this;
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

	timer_tick_t GetCurTick()				{	return _cur_tick;	}
	const struct timeval& GetCurTickTime()	{	return _cur_tv;		}
	time_t		GetTime()					{	return _cur_time; }
	const tm&	GetTm()						{	return _cur_tm; }
	int			GetTickOffsetMS()			{	return _tick_offset_ms; }

	uint32_t milling2tick(int millisecond)
	{
		int offset = millisecond / _tick_offset_ms;
		//定时器时间向tick时间修正取整
		offset += millisecond % _tick_offset_ms > 0 ? 1 : 0;
		return offset;
	}

	void AddTimer(BaseTimer* pTimer, int millisecond);

	template<typename F>
	typename std::enable_if<!std::is_base_of<BaseTimer, typename std::remove_pointer<typename std::decay<F>::type>::type>::value, void>::type
	AddTimer(F fn, int millisecond)
	{
		typedef typename std::decay<F>::type Fn;
		AddTimer(new FnTimer<Fn>(std::move(fn)), millisecond);
	}

	bool RemoveTimer(BaseTimer* pTimer)
	{
		OI::Thread::MutexGuard lock(_wheel_mu);
		assert(pTimer->GetTimerLevel());
		return pTimer->GetTimerLevel()->RemoveTimer(this, pTimer);
	}
	std::string GenTraceLog();
private:
	// 墙上时间到到期点还要几个 tick。不满一跳的零头留着，避免提前回调。
	int64_t TicksFromWall(int millisecond) const;

	//增加定时器
	void add_timer(BaseTimer* pTimer);
	//执行定时器
	void run_timer(BaseTimer* pTimer);
	//实际执行1tick的逻辑,
	void run_tick();
	//均衡高级定时器
	void try_uniform();
	//执行最低等级的一级定时器 _timer_level[0].RunTick()
	void try_run_timer();

	void collect_timer_trigger_delta(BaseTimer* pTimer)
	{
		/*
				struct timeval tv;
				gettimeofday(&tv, 0); //记录当前触发tick的实际物理时间
		*/
	}



private:
	friend class TimerLevel;
	OI::Thread::Mutex _wheel_mu;

	// 已经执行完的最后一个 tick 的逻辑时间。不满一跳的毫秒留在和墙上时间的差里，下次补齐。
	struct timeval		_last_tick_tv;

	// 最近一次 tick 的逻辑时间。这一跳还没满时，Update 会先写成墙上时间。
	struct timeval		_cur_tv;

	//当前tick数
	timer_tick_t		_cur_tick;
	//每tick的时间间隔
	uint32_t			_tick_offset_ms;
	//timer层级数
	size_t				_timer_lv_count;
	//当前timer层级数组,0代表与tick同步的定时器列表
	TimerLevelVector	_timer_levels;		//分级的timer
	bool				_trigger;

	time_t				_cur_time;
	struct tm			_cur_tm;
};