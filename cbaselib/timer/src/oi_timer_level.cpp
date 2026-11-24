#include "oi_timer_level.h"
#include "oi_timer_manager.h"
#include "oi_lock.h"
namespace OI
{
namespace Timer
{

void TimerLevel::RunTick(TimerList& vExpired)
{
	assert(m_iOffset >= 0 && m_iOffset < m_iMaxTick);
	::OI::Thread::MutexGuard stLock(m_stLocker);
	vExpired.splice(vExpired.end(), m_vTimerTickDatas[m_iOffset]);
	if (++m_iOffset == m_iMaxTick)
	{
		m_iOffset = 0;
	}
}
}
}
