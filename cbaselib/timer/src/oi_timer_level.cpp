#include "oi_timer_level.h"
#include "oi_timer_manager.h"
namespace OI
{
namespace Timer
{

void TimerLevel::RunTick(TimerList& vExpired)
{
	assert(m_iOffset >= 0 && m_iOffset < m_iMaxTick);
	vExpired.splice(vExpired.end(), m_vTimerTickDatas[m_iOffset]);
	if (++m_iOffset == m_iMaxTick)
	{
		m_iOffset = 0;
	}
}
}
}
