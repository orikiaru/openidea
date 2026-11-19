#include "oi_timer.h"
#include "oi_timer_manager.h"
namespace OI
{
namespace Timer
{

void BaseTimer::InitData(int iMillsec, timer_tick_t iTriggerTick, timeval stNow, int64_t iDelayMilliseconds)
{
	assert(iMillsec > 0);
	assert(iDelayMilliseconds > 0);

	m_stData.m_iOffsetMilliseconds = iMillsec;
	m_stData.m_iTriggerTick = iTriggerTick;
	m_stData.m_stTriggerTimestamp = stNow;
	int64_t iUs = (int64_t)m_stData.m_stTriggerTimestamp.tv_sec * 1000000 + (int64_t)m_stData.m_stTriggerTimestamp.tv_usec;
	iUs += iDelayMilliseconds * 1000;
	m_stData.m_stTriggerTimestamp.tv_sec = (decltype(m_stData.m_stTriggerTimestamp.tv_sec))(iUs / 1000000);
	m_stData.m_stTriggerTimestamp.tv_usec = (decltype(m_stData.m_stTriggerTimestamp.tv_usec))(iUs % 1000000);
}
}
}