#include <unistd.h>
#include <atomic>
#include "oi_timer_manager.h"
#include "oi_thread.h"
#include "oi_poll.h"
namespace OI { namespace Timer {

static std::atomic<bool> g_tick_queued(false);

static void AddMs(timeval& tv, int64_t ms)
{
	int64_t us = (int64_t)tv.tv_sec * 1000000 + (int64_t)tv.tv_usec + ms * 1000;
	tv.tv_sec = (decltype(tv.tv_sec))(us / 1000000);
	tv.tv_usec = (decltype(tv.tv_usec))(us % 1000000);
}

static void OnLoopTick()
{
	BaseTimerManager::GetInstance().OnSignal();
}

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
	(void)delta_us;

	uint32_t timer_lv_count = 4;
	TryGetConfigNoReturn("Timer.timer_lv_count", timer_lv_count);
	_timer_lv_count = timer_lv_count;
	assert(timer_lv_count > 2 && "timer_lv_count Must be bigger than 2");

	std::vector<int64_t> timer_lvs;
	if (!::OI::Base::Config::GlobalConfig().GetArrayAs("Timer.timer_lvs", timer_lvs))
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

	uint32_t lv_tick = 1;
	for (size_t i = 0; i < _timer_lv_count; ++i)
	{
		_timer_levels.emplace_back();
		uint32_t slots = (uint32_t)timer_lvs[i];
		_timer_levels[i].Init(slots, lv_tick);
		lv_tick *= slots;
	}

	_tick_offset_ms = offset_ms;
	gettimeofday(&_last_tick_tv, 0); //记录第0帧的时间
	gettimeofday(&_cur_tv, 0);	 //记录第0帧的时间
	_cur_time = time(NULL);
	localtime_r(&_cur_time, &_cur_tm);
	// Windows / Linux 都用 poll 循环上的 libuv timer 叫醒
	::OI::Net::Poll::GetInstance().ArmTick(_tick_offset_ms, OnLoopTick);
	return true;
}

void BaseTimerManager::OnSignal()
{
	bool expected = false;
	if (!g_tick_queued.compare_exchange_strong(expected, true))
	{
		return;
	}
	::OI::Thread::ThreadPool::GetInstance().AddTask([]() {
		BaseTimerManager::GetInstance().Update();
		g_tick_queued.store(false);
	}, ::OI::Thread::PRIO_ENUM::PRIO_QUEUE);
}

std::string BaseTimerManager::GenTraceLog()
{
	char buff[1024];
	snprintf(buff, sizeof(buff) - 1, 
		"BaseTimerManager::Update l_tick=[%8lld,%8lld] c_tick=[%8lld,%8lld] tick_num=[%8lld] offset=[%4u|%4u,%4u|%4u,%4u|%4u,%4u|%4u]",
		(long long)_last_tick_tv.tv_sec, (long long)_last_tick_tv.tv_usec,
		(long long)_cur_tv.tv_sec, (long long)_cur_tv.tv_usec,
		(long long)_cur_tick,
		_timer_levels[0]._offset, _timer_levels[0]._max_tick,
		_timer_levels[1]._offset, _timer_levels[1]._max_tick,
		_timer_levels[2]._offset, _timer_levels[2]._max_tick,
		_timer_levels[3]._offset, _timer_levels[3]._max_tick
	);
	return buff;
}

void BaseTimerManager::Update()
{
	::OI::Thread::MutexGuard lock(_wheel_mu);
	gettimeofday(&_cur_tv, 0);

	int64_t delta_us = (int64_t)_cur_tv.tv_sec - (int64_t)_last_tick_tv.tv_sec;
	delta_us *= 1000000;
	delta_us += (int64_t)_cur_tv.tv_usec - (int64_t)_last_tick_tv.tv_usec;
	if (delta_us <= 0)
	{
		_cur_time = _cur_tv.tv_sec;
		localtime_r(&_cur_time, &_cur_tm);
		return;
	}

	// 只取完整的 tick。剩下的毫秒留在 _last_tick_tv 和墙上时间的差里。
	int64_t ticks = (delta_us / 1000) / (int64_t)_tick_offset_ms;
	for (int64_t i = 0; i < ticks; ++i)
	{
		run_tick();
	}
	if (ticks == 0)
	{
		_cur_time = _cur_tv.tv_sec;
		localtime_r(&_cur_time, &_cur_tm);
	}
}

void BaseTimerManager::AddTimer(BaseTimer* pTimer, int millisecond)
{
	::OI::Thread::MutexGuard lock(_wheel_mu);
	pTimer->InitData(this, millisecond, TicksFromWall(millisecond));
	add_timer(pTimer);
}

int64_t BaseTimerManager::TicksFromWall(int millisecond) const
{
	struct timeval now;
	gettimeofday(&now, 0);
	int64_t elapsed_us = ((int64_t)now.tv_sec - (int64_t)_last_tick_tv.tv_sec) * 1000000;
	elapsed_us += (int64_t)now.tv_usec - (int64_t)_last_tick_tv.tv_usec;
	if (elapsed_us < 0)
	{
		elapsed_us = 0;
	}
	int64_t due_ms = elapsed_us / 1000 + millisecond;
	int64_t ticks = (due_ms + (int64_t)_tick_offset_ms - 1) / (int64_t)_tick_offset_ms;
	if (ticks < 1)
	{
		ticks = 1;
	}
	return ticks;
}

void BaseTimerManager::add_timer(BaseTimer* pTimer)
{
	int64_t offset = pTimer->_data._trigger_tick - _cur_tick;
	// 粗层倒出来时可能已经过了点。放到下一跳，避免在均匀过程里再进回调。
	if (offset <= 0)
	{
		offset = 1;
	}

	bool added = false;
	for (uint32_t i = 0; i < _timer_lv_count - 1; ++i)
	{
		TimerLevel& cur_level = _timer_levels[i];
		int64_t level_offset = offset / (int64_t)cur_level._lv_offset_tick;
		assert(level_offset >= 1);
		level_offset -= 1;
		if (level_offset >= (int64_t)cur_level._max_tick)
		{
			continue;
		}

		cur_level.AddTimer(this, pTimer, (uint32_t)level_offset);
		added = true;
		break;
	}

	if (!added)
	{
		TimerLevel& cur_level = _timer_levels[_timer_lv_count - 1];
		cur_level.AddTimer(this, pTimer, 0);
	}
}

void BaseTimerManager::run_timer(BaseTimer* pTimer)
{
	assert(pTimer);

	//collect_timer_trigger_delta(pTimer);

	if (pTimer->OnTimer())
	{
		// 周期定时器按逻辑 tick 续，停顿时后面的跳还能补上。
		int64_t ticks = milling2tick(pTimer->GetOffsetTime());
		pTimer->InitData(this, pTimer->GetOffsetTime(), ticks);
		add_timer(pTimer);
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
	AddMs(_last_tick_tv, _tick_offset_ms);
	++_cur_tick;
	_cur_tv = _last_tick_tv;
	_cur_time = _cur_tv.tv_sec;
	localtime_r(&_cur_time, &_cur_tm);

	try_run_timer();

	//第一层如果到尾,需要触发上级均匀
	if (_timer_levels[0].IsLevelEnd())
	{
		try_uniform();
	}
}
}}