#pragma once
#include "oi_errno.h"
#include <sys/time.h>
#include <string.h>
#include <list>
#include <algorithm>

#include "oi_lock.h"
#include "oi_timer.h"
#include <assert.h>
namespace OI
{
namespace Timer
{
class TimerLevel;
typedef std::list<BaseTimer*> TimerList;
typedef std::vector<TimerList> TimerListVector;

//只能通过TimerManager驱动,所以均为私有
class TimerLevel
{
	friend class BaseTimerManager;
	uint32_t m_iMaxTick;               // 这一层有多少个槽。
	uint32_t m_iLevelOffsetTick;       // 一个槽代表多少基础 tick。第 0 层是 1。
	uint32_t m_iOffset;                // 当前转到哪个槽。每跳加一，绕回数组开头。
	TimerListVector m_vTimerTickDatas; // 每个槽一条定时器链表。
	::OI::Thread::Mutex m_stLocker;    // 保护这一层的槽。和管理器的时间轮锁分开。

  public:
	TimerLevel()
	    : m_iMaxTick(0), m_iOffset(0)
	{
	}
	TimerLevel(const TimerLevel&) = delete;
	TimerLevel& operator=(const TimerLevel&) = delete;

  private:
	void Init(uint32_t iMaxTick, uint32_t iLevelOffsetTick)
	{
		assert(iLevelOffsetTick > 0);
		m_iLevelOffsetTick = iLevelOffsetTick;

		assert(iMaxTick > 0);
		m_iMaxTick = iMaxTick;
		for (uint32_t iIndexLocal = 0; iIndexLocal < iMaxTick; ++iIndexLocal)
		{
			m_vTimerTickDatas.push_back(TimerList()); //初始化定时器队列
		}
	}

	// due 不为空时把这一格移进 due，否则逐个重新挂到下层。
	void RunTick(BaseTimerManager* pMgr, TimerList* pDue);

	void AddTimer(BaseTimerManager* pManager, BaseTimer* pTimer, uint32_t iLevelOffset)
	{
		assert(iLevelOffset >= 0 && iLevelOffset < m_iMaxTick);

		::OI::Thread::MutexGuard stLock(m_stLocker);
		//在此定时器层级,则进入适当索引的定时器列表中
		int iOffset = (iLevelOffset + m_iOffset) % m_iMaxTick;
		m_vTimerTickDatas[iOffset].push_back(pTimer);
		pTimer->m_stData.m_pCurrentLevel = this;
		pTimer->m_stData.m_iLevelTick = iOffset;
	}

	//通常情况下, timer移除建议在OnTimer返回false移除
	//在多线程情况下,可能会存在移除过程中, timer被触发的情况, 此时移除将是失败的操作
	int32_t RemoveTimer(BaseTimerManager* pManager, BaseTimer* pTimer)
	{
		assert(pTimer->GetLevelOffset() >= 0 && pTimer->GetLevelOffset() < m_iMaxTick);

		::OI::Thread::MutexGuard stLock(m_stLocker);
		TimerList& vTimerList = m_vTimerTickDatas[pTimer->GetLevelOffset()];
		auto stIterator = std::find(vTimerList.begin(), vTimerList.end(), pTimer);
		if (stIterator != vTimerList.end())
		{
			vTimerList.erase(stIterator);
			return OI_OK;
		}
		return OI_ERR_NOT_FOUND;
	}

	//是否跑完一次循环
	bool IsLevelEnd()
	{
		return m_iOffset == 0;
	}
};
}
}
