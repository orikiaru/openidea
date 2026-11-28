#pragma once
#include "oi_timer_manager.h"

namespace OI::Timer
{
// 框架对象用裸指针借用，或用 unique_ptr 把销毁责任交给时间轮。
class FrameworkTimer
{
  public:
	// 工作线程和 poll 都已退出，锁外释放时间轮持有的所有对象。
	static void ReleasePendingAfterJoin();
	static int32_t AddTimer(BaseTimer* pTimer, int iIntervalMilliseconds)
	{
		return BaseTimerManager::GetInstance().RegisterTimer(TimerEntry(pTimer), iIntervalMilliseconds);
	}
	static int32_t AddTimer(std::unique_ptr<BaseTimer> pTimer, int iIntervalMilliseconds)
	{
		return BaseTimerManager::GetInstance().RegisterTimer(TimerEntry(std::move(pTimer)), iIntervalMilliseconds);
	}
};
}
