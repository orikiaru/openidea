#pragma once
#include <sys/time.h>
#include <string.h>
#include "header.h"
#include <list>
#include <vector>
#include <time.h>
#include "base_timer_level.h"
#include <assert.h>
#include <string>

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

typedef std::vector<TimerLevel> TimerLevelVector;

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
	//闹钟信号触发后处理,设置标记,唤醒线程
	void OnSignal();
	//定时器实际触发接口,单线程由PollAfterTask触发,多线程直接由timer线程触发
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

	bool RemoveTimer(BaseTimer* pTimer)
	{
		assert(pTimer->GetTimerLevel());
		return pTimer->GetTimerLevel()->RemoveTimer(this, pTimer);
	}
	//屏蔽闹钟信号，仅在单线程有用
	void Suspend();
	//唤醒闹钟信号，仅在单线程有用
	void Resume();
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

	sigset_t			_sig_set;
#ifndef _SINGLE_THREAD_
	pthread_t			_timer_thread_id;
#endif
};