#include <signal.h>
#include <unistd.h>
#include "base_timer_manager.h"
#include "threadpool.h"
#include "poll.h"

//单多线程都需要定时器实际触发后,在进行tick处理
class TimerManagerSignalTask : public Task
{
public:
	TimerManagerSignalTask() :Task(PRIO_ENUM::PRIO_NORMAL) {}
	void Exec()
	{
		BaseTimerManager::GetInstance()->OnSignal();
#ifdef _SINGLE_THREAD_
#else
		BaseTimerManager::GetInstance()->Update();
#endif
	}
};

//单线程时由PollTask驱动
class TimerManagerUpdateTask : public Task
{
public:
	TimerManagerUpdateTask() :Task(PRIO_ENUM::PRIO_NORMAL){}
	void Exec()
	{
		BaseTimerManager::GetInstance()->Resume();
	}
};

//单线程时由PollTask驱动前 需要屏蔽闹钟信号
class TimerManagerSuspendTask : public Task
{
public:
	TimerManagerSuspendTask() :Task(PRIO_ENUM::PRIO_NORMAL) {}
	void Exec()
	{
		BaseTimerManager::GetInstance()->Suspend();
	}
};

#ifndef _SINGLE_THREAD_
void* TimerThreadFunc(void* arg)
{
	ThreadPool::InitIgnoreSig();
	while (1)
	{
		BaseTimerManager::GetInstance()->Update();
		usleep(1000);
	}
}
#endif

BaseTimerManager::BaseTimerManager()
{
	memset(&_last_tick_tv, 0, sizeof(_last_tick_tv));
	memset(&_cur_tv, 0, sizeof(_cur_tv));
	_cur_tick = 0;
	_tick_offset_ms = 0;
	_trigger = false;


}

bool BaseTimerManager::Init()
{
	uint32_t offset_ms = 50;
	TryGetConfigNoReturn("Timer.offset_ms", offset_ms);
	uint32_t delta_us = offset_ms *1000;
	assert(1000000 % delta_us == 0 && "delta_us Must be divisible by 1000");

	uint32_t timer_lv_count = 4;
	TryGetConfigNoReturn("Timer.timer_lv_count", timer_lv_count);
	_timer_lv_count = timer_lv_count;
	assert(timer_lv_count > 2 && "timer_lv_count Must be bigger than 2");

	std::vector<int64_t> timer_lvs;
	if (!Config::GlobalConfig().GetArrayAs("Timer.timer_lvs", timer_lvs))
	{
		//没有就给一组默认数据,影响不大.
		std::vector<int64_t> tmp;
		for (size_t i = 0; i < _timer_lv_count; ++i)
		{
			tmp.push_back(128);
		}
		timer_lvs.swap(tmp);
		timer_lvs[_timer_lv_count - 1] = 1;
	}

	assert(timer_lvs[_timer_lv_count - 1] == 1 && "定时器最后一级一定是1");
	assert(timer_lvs.size() == _timer_lv_count && "配置跟总数一定要对齐");

	int lv_tick = 1;
	for (size_t i = 0; i < _timer_lv_count; ++i)
	{
		TimerLevel lv;
		_timer_levels.push_back(lv);
		_timer_levels[i].Init(timer_lvs[i], lv_tick);
		lv_tick *= timer_lvs[i];
	}

	sigemptyset(&_sig_set);
	sigaddset(&_sig_set, SIGALRM);
	PTHREAD_SIGMASK(SIG_BLOCK, &_sig_set, NULL);

	struct itimerval interval;
	interval.it_interval.tv_sec = delta_us / 1000000;
	interval.it_interval.tv_usec = delta_us % 1000000;
	interval.it_value.tv_sec = delta_us / 1000000;
	interval.it_value.tv_usec = delta_us % 1000000;
	setitimer(ITIMER_REAL, &interval, NULL);

	_tick_offset_ms = offset_ms;
	gettimeofday(&_last_tick_tv, 0); //记录第0帧的时间
	gettimeofday(&_cur_tv, 0);	 //记录第0帧的时间
	_cur_time = time(NULL);
	localtime_r(&_cur_time, &_cur_tm);
	//单线程时需要注册PollAfterTask来驱动定时器逻辑,信号中断会触发io回执
#ifdef _SINGLE_THREAD_
	Poll::GetInstance()->AddPollTask(new TimerManagerUpdateTask(), Poll::PollTaskPosEnum::POLL_AFTER_TASK, false);
	Poll::GetInstance()->AddPollTask(new TimerManagerSuspendTask(), Poll::PollTaskPosEnum::POLL_BEFORE_TASK, false);
#endif
	//多线程下，交给主线程处理
	ThreadPool::GetInstance()->AddSigHandleTask(SIGALRM, new TimerManagerSignalTask());
	return true;
}

void BaseTimerManager::OnSignal()
{
	_trigger = true;
}

std::string BaseTimerManager::GenTraceLog()
{
	char buff[1024];
	snprintf(buff, sizeof(buff) - 1, 
		"BaseTimerManager::Update l_tick=[%8ld,%8ld] c_tick=[%8ld,%8ld] tick_num=[%8ld] offset=[%4d|%4d,%4d|%4d,%4d|%4d,%4d|%4d]",
		_last_tick_tv.tv_sec, _last_tick_tv.tv_usec,
		_cur_tv.tv_sec, _cur_tv.tv_usec,
		_cur_tick,
		_timer_levels[0]._offset, _timer_levels[0]._max_tick,
		_timer_levels[1]._offset, _timer_levels[1]._max_tick,
		_timer_levels[2]._offset, _timer_levels[2]._max_tick,
		_timer_levels[3]._offset, _timer_levels[3]._max_tick
	);
	return buff;
}

void BaseTimerManager::Update()
{
	_cur_time = time(NULL);
	localtime_r(&_cur_time, &_cur_tm);
	gettimeofday(&_cur_tv, 0);

	//计算tick数,触发tick
	int offset = get_offset_tick();
	for (int i = 0; i < offset; ++i)
	{
		run_tick();
	}
	//struct timeval t;
	//gettimeofday(&t, 0);
	//printf("%s\n", GenTraceLog().c_str());
}

void BaseTimerManager::Suspend()
{
	//单线程时,由IO线程事件回调后触发,所以可能存在没有触发定时器消息的情况
	PTHREAD_SIGMASK(SIG_BLOCK, &_sig_set, NULL);
	if (_trigger)
	{
		Update();
		_trigger = false;
	}
}

void BaseTimerManager::Resume()
{
	PTHREAD_SIGMASK(SIG_UNBLOCK, &_sig_set, NULL);
}

void BaseTimerManager::add_timer(BaseTimer* pTimer)
{
	int offset = pTimer->_data._trigger_tick - _cur_tick;
	if (offset <= 0)
	{
		//TODO极端情况下可能存在, add_timer的时候进行了2次run_tick
		offset = 1;
	}

	bool added = false;
	//处理前 _timer_lv_count - 1 个固定分散层级
	for (uint32_t i = 0; i < _timer_lv_count - 1; ++i)
	{
		TimerLevel& cur_level = _timer_levels[i];
		uint32_t level_offset = offset / _timer_levels[i]._lv_offset_tick;
		assert(level_offset >= 1);
		level_offset -= 1;
		//不在此定时器层级
		if (level_offset >= _timer_levels[i]._max_tick)
		{
			continue;
		}

		//此处选择超过_max_tick才进行跨级,而不是采用 max_tick-offset跨级,是为了降低临近队列尾时的timer数量的不可控性
		//大于一轮会丢到下一级，可能会被提前均匀，不过问题不大，大不了均匀时回到同级同offset的队列，这样可以保证每个offset队列均匀

		//TODO 此处可能外部已经切换，比如第二级遍历完，导致第三级越界到下一个offset，此时加入的定时器可能会延时一整个第二级的执行时间
		//TODO 尤其在多线程调试状态下，极其不稳定。
		//TODO 锁粒度加大？

		cur_level.AddTimer(this, pTimer, level_offset);
		added = true;
		break;
	}

	//TODO 目前这种只在TimerLevel层级加小锁的情况,极端情况下可能引起Timer加入到timer时,正在切换,导致均匀被拖延一个tick
	//因为现在分层至少1s,所以不太可能存在拖延n个tick情况.

	//例: 比如虽然上面计算要加入到第二个层级,第一个,不过第二层已经RunTick,切换到下一层,这样这个timer可能会被延后一个或n个tick
	//因为加入timer时,是按照当前offset计算,此时offset可能已经偏移
	//毕竟计算时间时,是获取的本次触发逻辑时间的快照,并且无锁保护
	//不过应该可以接受.

	//没有入前三个分散层级,那么进入全队列
	if (!added)
	{
		TimerLevel& cur_level = _timer_levels[_timer_lv_count - 1];
		cur_level.AddTimer(this, pTimer, 0);
	}
}

int BaseTimerManager::get_offset_tick()
{
	int offset = cal_timeval_delta(_last_tick_tv, _cur_tv);
	suseconds_t offset_us = offset * 1000;
	offset /= _tick_offset_ms;

	_last_tick_tv.tv_usec += offset_us;
	_last_tick_tv.tv_sec += _last_tick_tv.tv_usec / 1000000;
	_last_tick_tv.tv_usec %= 1000000;
	return offset;
}

void BaseTimerManager::run_timer(BaseTimer* pTimer)
{
	assert(pTimer);

	//collect_timer_trigger_delta(pTimer);

	if (pTimer->OnTimer())
	{
		AddTimer(pTimer, pTimer->GetOffsetTime());
	}
}

void BaseTimerManager::try_uniform()
{
	//尝试均匀其他层级定时器
	for (uint32_t i = 1; i < _timer_lv_count; ++i)
	{
		if (_timer_levels[i - 1].IsLevelEnd())
		{
			//先执行tick,以便均匀后的定时器进入准确的offset
			_timer_levels[i].RunTick(this, false);
		}
	}
}

void BaseTimerManager::try_run_timer()
{
	//执行本tick逻辑
	TimerLevel& level = _timer_levels[0];
	level.RunTick(this, true);
}

void BaseTimerManager::run_tick()
{
	++_cur_tick;

	try_run_timer();

	//第一层如果到尾,需要触发上级均匀
	if (_timer_levels[0].IsLevelEnd())
	{
		try_uniform();
	}
}