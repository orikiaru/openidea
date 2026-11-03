#pragma once
#include <sys/time.h>
#include <string.h>
#include "oi_header.h"
#include <list>
#include <vector>
#include <time.h>
#include "oi_header.h"
namespace OI { namespace Timer {

class BaseTimer;
class BaseTimerManager;
class TimerLevel;
typedef int64_t timer_tick_t;

enum
{
	RESET_TIMER = true,
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
		struct timeval	m_stTriggerTimestamp;	//触发时戳
		int32_t			m_iOffsetMilliseconds;		//间隔ms数
		timer_tick_t	m_iTriggerTick;	//偏移基础tick数
		TimerLevel*		m_pCurrentLevel;		//当前所处的timer_lv
		uint32_t		m_iLevelTick;	//所在timer_lv的offset

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
	TimerData m_stData;		//标记定时器所在定时器管理器的位置,只由TimerManager控制,外部不可操作
	void InitData(BaseTimerManager* pMgrParam, int iMillsecParam, int64_t iTicksParam);
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
	BaseTimer() {}
	virtual ~BaseTimer() {}
	virtual bool OnTimer() = 0;
};
}}




