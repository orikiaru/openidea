#pragma once
#include <sys/time.h>
#include <string.h>
#include "header.h"
#include <list>
#include <vector>
#include <time.h>
#include "header.h"

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
		struct timeval	_trigger_ts;	//触发时戳
		int				_offset_ms;		//间隔ms数
		timer_tick_t	_trigger_tick;	//偏移基础tick数
		TimerLevel*		_cur_level;		//当前所处的timer_lv
		uint32_t		_level_tick;	//所在timer_lv的offset

		TimerData()
		{
			memset(&_trigger_ts, 0, sizeof(_trigger_ts));
			_offset_ms = 0;
			_trigger_tick = 0;
			_cur_level = NULL;
			_level_tick = -1;
			_level_tick = 0;
		}
	};

	friend class BaseTimerManager;
	TimerData _data;		//标记定时器所在定时器管理器的位置,只由TimerManager控制,外部不可操作
	void InitData(BaseTimerManager* pMgr, int millsec);
	TimerLevel* GetTimerLevel()
	{
		return _data._cur_level;
	}
	uint32_t GetLevelOffset()
	{
		return _data._level_tick;
	}
	int GetOffsetTime()
	{
		return _data._offset_ms;
	}
	struct timeval& GetTriggerTime()
	{
		return _data._trigger_ts;
	}

public:
	BaseTimer() {}
	virtual ~BaseTimer() {}
	virtual bool OnTimer() = 0;
};




