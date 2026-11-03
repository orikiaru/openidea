#include "oi_timer.h"
#include "oi_timer_manager.h"
namespace OI { namespace Timer {

void BaseTimer::InitData(BaseTimerManager* pMgrParam, int iMillsecParam, int64_t iTicksParam)
{
	assert(iMillsecParam > 0);
	assert(iTicksParam > 0);

	m_stData.m_iOffsetMilliseconds = iMillsecParam;
	m_stData.m_iTriggerTick = pMgrParam->GetCurTick() + iTicksParam;
	m_stData.m_stTriggerTimestamp = pMgrParam->GetCurTickTime();
	int64_t iUs = (int64_t)m_stData.m_stTriggerTimestamp.tv_sec * 1000000 + (int64_t)m_stData.m_stTriggerTimestamp.tv_usec;
	iUs += iTicksParam * (int64_t)pMgrParam->GetTickOffsetMS() * 1000;
	m_stData.m_stTriggerTimestamp.tv_sec = (decltype(m_stData.m_stTriggerTimestamp.tv_sec))(iUs / 1000000);
	m_stData.m_stTriggerTimestamp.tv_usec = (decltype(m_stData.m_stTriggerTimestamp.tv_usec))(iUs % 1000000);
}
}}