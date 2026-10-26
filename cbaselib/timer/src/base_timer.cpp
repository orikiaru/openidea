#include "base_timer.h"
#include "base_timer_manager.h"

void BaseTimer::InitData(BaseTimerManager* pMgr, int millsec)
{

	assert(millsec > 0);
	int offset = pMgr->milling2tick(millsec);

	//赋值定时器基础属性
	_data._offset_ms = millsec;
	_data._trigger_tick = pMgr->GetCurTick() + offset;
	_data._trigger_ts = pMgr->GetCurTickTime();
	_data._trigger_ts.tv_usec += millsec * 1000;
	_data._trigger_ts.tv_sec += _data._trigger_ts.tv_usec / 1000000;
	_data._trigger_ts.tv_usec = _data._trigger_ts.tv_usec % 1000000;
}