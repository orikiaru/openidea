#include "oi_timer_level.h"
#include "oi_timer_manager.h"
#include "oi_lock.h"
namespace OI { namespace Timer {

void TimerLevel::RunTick(BaseTimerManager* pMgr, TimerList* due)
{
	assert(_offset >= 0 && _offset < _max_tick);
	TimerList cur_list;
	{
		::OI::Thread::MutexGuard l(_locker);

		cur_list.swap(_timer_tick_datas[_offset]);

		++_offset;
		if (_offset >= _max_tick)
		{
			assert(_offset == _max_tick);
			_offset = 0;
		}
	}

	// 第一层到点的交给调用方在锁外执行，上层的按剩余 tick 重新挂到下层。
	if (due)
	{
		due->splice(due->end(), cur_list);
		return;
	}
	for (TimerList::iterator it = cur_list.begin(); it != cur_list.end(); ++it)
	{
		pMgr->add_timer(*it);
	}
}
}}
