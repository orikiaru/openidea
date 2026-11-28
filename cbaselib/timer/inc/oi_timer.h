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

// 只在本次回调中借用。次数表示当前回调返回后还要执行几次。
class TimerContext
{
	template <typename F>
	friend class FnTimer;
	int32_t m_iRemaining;

	explicit TimerContext(int32_t iRemaining)
	    : m_iRemaining(iRemaining)
	{
	}

  public:
	TimerContext(const TimerContext&) = delete;
	TimerContext& operator=(const TimerContext&) = delete;

	int32_t Remaining() const
	{
		return m_iRemaining;
	}
	void SetRemaining(int32_t iRemaining)
	{
		m_iRemaining = iRemaining > 0 ? iRemaining : 0;
	}
	void Stop()
	{
		m_iRemaining = 0;
	}
};

class BaseTimer;
class BaseTimerManager;
class TimerLevel;
typedef int64_t timer_tick_t;

enum
{
	// OnTimer 返回它：按原来的间隔再挂上。
	RESET_TIMER = true,
	// OnTimer 返回它：从时间轮摘掉，并释放时间轮持有的对象。
	END_TIMER = false,
};
// 借用登记的对象必须活到回调结束或时间轮停服。
class BaseTimer
{
	class TimerData
	{
		friend class BaseTimer;
		friend class BaseTimerManager;
		struct timeval m_stTriggerTimestamp; // 登记时的日历时间，只给诊断看，不参与到点判断。
		int32_t m_iOffsetMilliseconds;       // 相对间隔。返回 RESET_TIMER 时按它再挂。
		timer_tick_t m_iTriggerTick;         // 相对单调时钟还要等多少基础 tick。

		TimerData()
		{
			memset(&m_stTriggerTimestamp, 0, sizeof(m_stTriggerTimestamp));
			m_iOffsetMilliseconds = 0;
			m_iTriggerTick = 0;
		}
	};

	friend class BaseTimerManager;
	friend class TimerTestAccess;
	TimerData m_stData; // 到期时间和续期间隔。只由管理器改，外部不要动。
	// 写入间隔、目标 tick 和触发时间。AddTimer 和续期都走这里。
	void InitData(int iMillsec, timer_tick_t iTriggerTick, timeval stNow, int64_t iDelayMilliseconds);
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
