#pragma once
#include <sys/time.h>
#include <string.h>
#include <list>
#include <algorithm>

#include "spin_lock.h"
#include "base_timer.h"
#include <assert.h>
class TimerLevel;
typedef std::list<BaseTimer*>	TimerList;
typedef std::vector<TimerList>	TimerListVector;

//只能通过TimerManager驱动,所以均为私有
class TimerLevel
{
	friend class BaseTimerManager;
	uint32_t		_max_tick;					//当前等级的timer的tick数
	uint32_t		_lv_offset_tick;			//当前等级基础偏移tick数,第一级就是1，第二集就是1*lv[0]._max_tick =====
	uint32_t		_offset;					//当前数组偏移量
	TimerListVector _timer_tick_datas;			//定时器队列
	Mutex		_locker;
	TimerLevel():_max_tick(0),_offset(0) {}
	void Init(uint32_t max_tick, uint32_t lv_offset_tick)
	{
		assert(lv_offset_tick > 0);
		_lv_offset_tick = lv_offset_tick;

		assert(max_tick > 0);
		_max_tick = max_tick;
		for (uint32_t i = 0; i < max_tick; ++i)
		{
			_timer_tick_datas.push_back(TimerList());	//初始化定时器队列
		}
	}

	void RunTick(BaseTimerManager* pMgr, bool run_timer = false);

	void AddTimer(BaseTimerManager* mgr, BaseTimer* pTimer, uint32_t level_offset)
	{
		assert(level_offset >= 0 && level_offset < _max_tick);

		MutexGuard l(_locker);
		//在此定时器层级,则进入适当索引的定时器列表中
		int offset = (level_offset + _offset) % _max_tick;
		_timer_tick_datas[offset].push_back(pTimer);
		pTimer->_data._cur_level = this;
		pTimer->_data._level_tick = offset;
	}

	//通常情况下, timer移除建议在OnTimer返回false移除
	//在多线程情况下,可能会存在移除过程中, timer被触发的情况, 此时移除将是失败的操作
	bool RemoveTimer(BaseTimerManager* mgr, BaseTimer* pTimer)
	{
		assert(pTimer->GetLevelOffset() >= 0 && pTimer->GetLevelOffset() < _max_tick);

		MutexGuard l(_locker);
		TimerList& timer_list = _timer_tick_datas[pTimer->GetLevelOffset()];
		TimerList::iterator it = std::find(timer_list.begin(), timer_list.end(), pTimer);
		if (it != timer_list.end())
		{
			timer_list.erase(it);
			return true;
		}
		return false;
	}

	//是否跑完一次循环
	bool IsLevelEnd()
	{
		return _offset == 0;
	}
};
