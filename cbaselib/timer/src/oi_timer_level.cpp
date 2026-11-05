#include "oi_timer_level.h"
#include "oi_timer_manager.h"
#include "oi_lock.h"
namespace OI
{
namespace Timer
{

void TimerLevel::RunTick(BaseTimerManager* pMgr, TimerList* pDue)
{
	assert(m_iOffset >= 0 && m_iOffset < m_iMaxTick);
	TimerList vCurrentList;
	{
		::OI::Thread::MutexGuard stLock(m_stLocker);

		vCurrentList.swap(m_vTimerTickDatas[m_iOffset]);

		++m_iOffset;
		if (m_iOffset >= m_iMaxTick)
		{
			assert(m_iOffset == m_iMaxTick);
			m_iOffset = 0;
		}
	}

	// 第一层到点的交给调用方在锁外执行，上层的按剩余 tick 重新挂到下层。
	if (pDue)
	{
		pDue->splice(pDue->end(), vCurrentList);
		return;
	}
	for (const auto& pTimer : vCurrentList)
	{
		pMgr->add_timer(pTimer);
	}
}
}
}
