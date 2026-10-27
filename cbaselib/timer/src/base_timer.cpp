#include "base_timer.h"
#include "base_timer_manager.h"

void BaseTimer::InitData(BaseTimerManager* pMgr, int millsec, int64_t ticks)
{
	assert(millsec > 0);
	assert(ticks > 0);

	_data._offset_ms = millsec;
	_data._trigger_tick = pMgr->GetCurTick() + ticks;
	_data._trigger_ts = pMgr->GetCurTickTime();
	int64_t us = (int64_t)_data._trigger_ts.tv_sec * 1000000 + (int64_t)_data._trigger_ts.tv_usec;
	us += ticks * (int64_t)pMgr->GetTickOffsetMS() * 1000;
	_data._trigger_ts.tv_sec = (time_t)(us / 1000000);
	_data._trigger_ts.tv_usec = (suseconds_t)(us % 1000000);
}