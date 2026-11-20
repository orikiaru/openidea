#pragma once
#include "oi_timer_manager.h"

namespace OI::Timer
{
// 仅供框架自管生命周期的对象定时器使用。
class FrameworkTimer
{
  public:
	static int32_t AddTimer(BaseTimer* pTimer, int iIntervalMilliseconds)
	{
		return BaseTimerManager::GetInstance().RegisterTimer(pTimer, iIntervalMilliseconds);
	}
};
}
