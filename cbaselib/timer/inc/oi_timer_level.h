#pragma once
#include "oi_errno.h"
#include <sys/time.h>
#include <string.h>
#include <list>
#include <memory>
#include <utility>

#include "oi_timer.h"
#include <assert.h>
namespace OI
{
namespace Timer
{
// pOwner 仅在时间轮负责销毁对象时非空。
struct TimerEntry
{
	BaseTimer* pTimer;
	std::unique_ptr<BaseTimer> pOwner;

	explicit TimerEntry(BaseTimer* pBorrowed)
	    : pTimer(pBorrowed)
	{
	}
	explicit TimerEntry(std::unique_ptr<BaseTimer> pOwned)
	    : pTimer(pOwned.get()), pOwner(std::move(pOwned))
	{
	}
};

typedef std::list<TimerEntry> TimerList;
typedef std::vector<TimerList> TimerListVector;

//只能通过TimerManager驱动,所以均为私有
class TimerLevel
{
	friend class BaseTimerManager;
	uint32_t m_iMaxTick;               // 这一层有多少个槽。
	uint32_t m_iLevelOffsetTick;       // 一个槽代表多少基础 tick。第 0 层是 1。
	uint32_t m_iOffset;                // 当前转到哪个槽。每跳加一，绕回数组开头。
	TimerListVector m_vTimerTickDatas; // 每个槽一条定时器链表。

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
		m_vTimerTickDatas = TimerListVector(iMaxTick);
	}

	// 摘下当前槽，再推进一格。本次重新登记的任务不会进入已摘下的批次。
	void RunTick(TimerList& vExpired);

	TimerList& Slot(uint32_t iLevelOffset)
	{
		assert(iLevelOffset < m_iMaxTick);
		return m_vTimerTickDatas[(iLevelOffset + m_iOffset) % m_iMaxTick];
	}

	//是否跑完一次循环
	bool IsLevelEnd()
	{
		return m_iOffset == 0;
	}
};
}
}
