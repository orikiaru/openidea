#pragma once
#include <sys/time.h>
#include <string.h>
#include "oi_header.h"
#include <list>
#include <vector>
#include <time.h>
#include "oi_header.h"
namespace OI
{
namespace Timer
{

class BaseTimer;
class BaseTimerManager;
class TimerLevel;
typedef int64_t timer_tick_t;

enum
{
	// OnTimer 返回它：按原来的间隔再挂上。
	RESET_TIMER = true,
	// OnTimer 返回它：从时间轮摘掉，对象由调用方或回调自己销毁。
	END_TIMER = false,
};
//Timer必须从管理器中移除成功后,或者OnTimer返回false前销毁
class BaseTimer
{
	friend class TimerLevel;
	class TimerData
	{
		friend class BaseTimer;
		friend class TimerLevel;
		friend class BaseTimerManager;
		struct timeval m_stTriggerTimestamp; // 登记时的日历时间，只给诊断看，不参与到点判断。
		int32_t m_iOffsetMilliseconds;       // 相对间隔。返回 RESET_TIMER 时按它再挂。
		timer_tick_t m_iTriggerTick;         // 相对单调时钟还要等多少基础 tick。
		TimerLevel* m_pCurrentLevel;         // 当前挂在哪一层。没挂上时为空。
		uint32_t m_iLevelTick;               // 在这一层槽数组里的下标。RemoveTimer 靠它找到链表。

		TimerData()
		{
			memset(&m_stTriggerTimestamp, 0, sizeof(m_stTriggerTimestamp));
			m_iOffsetMilliseconds = 0;
			m_iTriggerTick = 0;
			m_pCurrentLevel = NULL;
			m_iLevelTick = 0;
		}
	};

	friend class BaseTimerManager;
	TimerData m_stData; // 在时间轮上的位置。只由管理器改，外部不要动。
	// 写入间隔、目标 tick 和触发时间。AddTimer 和续期都走这里。
	void InitData(BaseTimerManager* pMgr, int iMillsec, int64_t iTicks);
	TimerLevel* GetTimerLevel()
	{
		return m_stData.m_pCurrentLevel;
	}
	uint32_t GetLevelOffset()
	{
		return m_stData.m_iLevelTick;
	}
	int GetOffsetTime()
	{
		return m_stData.m_iOffsetMilliseconds;
	}
	struct timeval& GetTriggerTime()
	{
		return m_stData.m_stTriggerTimestamp;
	}

  public:
	BaseTimer()
	{
	}
	virtual ~BaseTimer()
	{
	}
	// 到点后在 poll 线程、不持时间轮锁时调用。返回 RESET_TIMER 再挂，返回 END_TIMER 结束。
	virtual bool OnTimer() = 0;
};
}
}
