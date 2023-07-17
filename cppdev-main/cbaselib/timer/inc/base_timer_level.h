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

//ֻ��ͨ��TimerManager����,���Ծ�Ϊ˽��
class TimerLevel
{
	friend class BaseTimerManager;
	uint32_t		_max_tick;					//��ǰ�ȼ���timer��tick��
	uint32_t		_lv_offset_tick;			//��ǰ�ȼ�����ƫ��tick��,��һ������1���ڶ�������1*lv[0]._max_tick =====
	uint32_t		_offset;					//��ǰ����ƫ����
	TimerListVector _timer_tick_datas;			//��ʱ������
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
			_timer_tick_datas.push_back(TimerList());	//��ʼ����ʱ������
		}
	}

	void RunTick(BaseTimerManager* pMgr, bool run_timer = false);

	void AddTimer(BaseTimerManager* mgr, BaseTimer* pTimer, uint32_t level_offset)
	{
		assert(level_offset >= 0 && level_offset < _max_tick);

		MutexGuard l(_locker);
		//�ڴ˶�ʱ���㼶,������ʵ������Ķ�ʱ���б���
		int offset = (level_offset + _offset) % _max_tick;
		_timer_tick_datas[offset].push_back(pTimer);
		pTimer->_data._cur_level = this;
		pTimer->_data._level_tick = offset;
	}

	//ͨ�������, timer�Ƴ�������OnTimer����false�Ƴ�
	//�ڶ��߳������,���ܻ�����Ƴ�������, timer�����������, ��ʱ�Ƴ�����ʧ�ܵĲ���
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

	//�Ƿ�����һ��ѭ��
	bool IsLevelEnd()
	{
		return _offset == 0;
	}
};
