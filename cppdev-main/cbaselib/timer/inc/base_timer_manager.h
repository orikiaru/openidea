#pragma once
#include <sys/time.h>
#include <string.h>
#include "header.h"
#include <deque>
#include <list>
#include <vector>
#include <time.h>
#include "base_timer_level.h"
#include "timer_task.h"
#include <assert.h>
#include <string>
#include <type_traits>
#include <utility>

//返回差异ms数
static int cal_timeval_delta(const timeval& old, const timeval& now)
{
	//assert(now.tm_sec > old.tm_sec || (now.tv_usec >= old.tv_usec && now.tm_sec == old.tm_sec);
	int64_t delta_us = now.tv_sec - old.tv_sec;
	delta_us *= 1000000;
	delta_us += now.tv_usec - old.tv_usec;
	assert(delta_us > 0);
	return delta_us/1000;
}

typedef std::deque<TimerLevel> TimerLevelVector;

class BaseTimerManager
{

public:
	BaseTimerManager();
	static BaseTimerManager* GetInstance()
	{
		static BaseTimerManager instance;
		return &instance;
	}
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

	void AddTimer(BaseTimer* pTimer, int millisecond)
	{
		pTimer->InitData(this, millisecond);
		add_timer(pTimer);
	}

	template<typename F>
	typename std::enable_if<!std::is_base_of<BaseTimer, typename std::remove_pointer<typename std::decay<F>::type>::type>::value, void>::type
	AddTimer(F fn, int millisecond)
	{
		typedef typename std::decay<F>::type Fn;
		AddTimer(new FnTimer<Fn>(std::move(fn)), millisecond);
	}

	bool RemoveTimer(BaseTimer* pTimer)
	{
		assert(pTimer->GetTimerLevel());
		return pTimer->GetTimerLevel()->RemoveTimer(this, pTimer);
	}
	std::string GenTraceLog();
private:
	int get_offset_tick();

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

	//记录上一次触发定时器的理论tick时间,用来做tick补偿 
	//比如某一tick事务过多,导致tick延时,那么下一个tick如果物理时间超过2tick,那么将两个tick一起执行
	struct timeval		_last_tick_tv;

	//这里是每次run_tick的实际物理时间快照
	//timer实际添加时,根据此tv做偏移计算?
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