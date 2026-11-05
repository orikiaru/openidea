#include "oi_timer.h"
#include "oi_timer_manager.h"
namespace OI
{
namespace Timer
{

void BaseTimer::InitData(BaseTimerManager* pMgr, int iMillsec, int64_t iTicks)
{
	assert(iMillsec > 0);
	assert(iTicks > 0);

	m_stData.m_iOffsetMilliseconds = iMillsec;
	m_stData.m_iTriggerTick = pMgr->GetCurTick() + iTicks;
	m_stData.m_stTriggerTimestamp = pMgr->GetCurTickTime();
	int64_t iUs = (int64_t)m_stData.m_stTriggerTimestamp.tv_sec * 1000000 + (int64_t)m_stData.m_stTriggerTimestamp.tv_usec;
	iUs += iTicks * (int64_t)pMgr->GetTickOffsetMS() * 1000;
	m_stData.m_stTriggerTimestamp.tv_sec = (decltype(m_stData.m_stTriggerTimestamp.tv_sec))(iUs / 1000000);
	m_stData.m_stTriggerTimestamp.tv_usec = (decltype(m_stData.m_stTriggerTimestamp.tv_usec))(iUs % 1000000);
}
}
}