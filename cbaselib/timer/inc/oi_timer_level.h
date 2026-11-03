#pragma once
#include <sys/time.h>
#include <string.h>
#include <list>
#include <algorithm>

#include "oi_lock.h"
#include "oi_timer.h"
#include <assert.h>
namespace OI { namespace Timer {
class TimerLevel;
typedef std::list<BaseTimer*>	TimerList;
typedef std::vector<TimerList>	TimerListVector;

//只能通过TimerManager驱动,所以均为私有
class TimerLevel
{
	friend class BaseTimerManager;
	uint32_t		m_iMaxTick;					//当前等级的timer的tick数
	uint32_t		m_iLevelOffsetTick;			//当前等级基础偏移tick数,第一级就是1，第二集就是1*lv[0]._max_tick =====
	uint32_t		m_iOffset;					//当前数组偏移量
	TimerListVector m_vTimerTickDatas;			//定时器队列
	::OI::Thread::Mutex		m_stLocker;
public:
	TimerLevel():m_iMaxTick(0),m_iOffset(0) {}
	TimerLevel(const TimerLevel&) = delete;
	TimerLevel& operator=(const TimerLevel&) = delete;
private:
	void Init(uint32_t iMaxTickParam, uint32_t iLevelOffsetTickParam)
	{
		assert(iLevelOffsetTickParam > 0);
		m_iLevelOffsetTick = iLevelOffsetTickParam;

		assert(iMaxTickParam > 0);
		m_iMaxTick = iMaxTickParam;
		for (uint32_t iIndexLocal = 0; iIndexLocal < iMaxTickParam; ++iIndexLocal)
		{
			m_vTimerTickDatas.push_back(TimerList());	//初始化定时器队列
		}
	}

	// due 不为空时把这一格移进 due，否则逐个重新挂到下层。
	void RunTick(BaseTimerManager* pMgrParam, TimerList* pDueParam);

	void AddTimer(BaseTimerManager* pManagerParam, BaseTimer* pTimerParam, uint32_t iLevelOffsetParam)
	{
		assert(iLevelOffsetParam >= 0 && iLevelOffsetParam < m_iMaxTick);

		::OI::Thread::MutexGuard stLock(m_stLocker);
		//在此定时器层级,则进入适当索引的定时器列表中
		int iOffset = (iLevelOffsetParam + m_iOffset) % m_iMaxTick;
		m_vTimerTickDatas[iOffset].push_back(pTimerParam);
		pTimerParam->m_stData.m_pCurrentLevel = this;
		pTimerParam->m_stData.m_iLevelTick = iOffset;
	}

	//通常情况下, timer移除建议在OnTimer返回false移除
	//在多线程情况下,可能会存在移除过程中, timer被触发的情况, 此时移除将是失败的操作
	bool RemoveTimer(BaseTimerManager* pManagerParam, BaseTimer* pTimerParam)
	{
		assert(pTimerParam->GetLevelOffset() >= 0 && pTimerParam->GetLevelOffset() < m_iMaxTick);

		::OI::Thread::MutexGuard stLock(m_stLocker);
		TimerList& vTimerList = m_vTimerTickDatas[pTimerParam->GetLevelOffset()];
		auto stIterator = std::find(vTimerList.begin(), vTimerList.end(), pTimerParam);
		if (stIterator != vTimerList.end())
		{
			vTimerList.erase(stIterator);
			return true;
		}
		return false;
	}

	//是否跑完一次循环
	bool IsLevelEnd()
	{
		return m_iOffset == 0;
	}
};
}}
