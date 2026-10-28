#include "oi_timer_level.h"
#include "oi_timer_manager.h"
#include "oi_lock.h"

void TimerLevel::RunTick(BaseTimerManager* pMgr, bool run_timer)
{
	assert(_offset >= 0 && _offset < _max_tick);
	TimerList cur_list;
	{
		OI::Thread::MutexGuard l(_locker);

		cur_list.swap(_timer_tick_datas[_offset]);

		++_offset;
		if (_offset >= _max_tick)
		{
			assert(_offset == _max_tick);
			_offset = 0;
		}
	}

#if __cplusplus >= 201103L
	//执行定时器走上面,均匀定时器走下面
	if (run_timer)
	{
		std::for_each(cur_list.begin(), cur_list.end(), [&](BaseTimer* pTimer) {	pMgr->run_timer(pTimer); });
	}
	else
	{
		std::for_each(cur_list.begin(), cur_list.end(), [&](BaseTimer* pTimer) {	pMgr->add_timer(pTimer); });
	}
#else
	typedef void (BaseTimerManager::* timer_run_func_t) (BaseTimer*);
	timer_run_func_t func = run_timer ? &BaseTimerManager::run_timer : &BaseTimerManager::add_timer;
	for (TimerList::iterator it = cur_list.begin(); it != cur_list.end(); ++it)
	{
		(pMgr->*func)(*it);
	}
#endif
}