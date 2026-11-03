#include <unistd.h>
#include <stdio.h>
#include <atomic>
#include "oi_timer_manager.h"
#include "oi_thread.h"
#include "oi_poll.h"
namespace OI { namespace Timer {

static std::atomic<bool> g_tick_queued(false);

static void OnLoopTick()
{
	BaseTimerManager::GetInstance().OnSignal();
}

BaseTimerManager::BaseTimerManager()
{
	memset(&_cur_tv, 0, sizeof(_cur_tv));
	_cur_tick = 0;
	_tick_offset_ms = 0;
}

bool BaseTimerManager::Init()
{
	uint32_t offset_ms = 50;
	TryGetConfigNoReturn("Timer.offset_ms", offset_ms);
	if (offset_ms == 0 || 1000 % offset_ms != 0)
	{
		fprintf(stderr, "Timer.offset_ms=%u must divide 1000\n", offset_ms);
		return false;
	}

	uint32_t timer_lv_count = 4;
	TryGetConfigNoReturn("Timer.timer_lv_count", timer_lv_count);
	if (timer_lv_count <= 2)
	{
		fprintf(stderr, "Timer.timer_lv_count=%u must be bigger than 2\n", timer_lv_count);
		return false;
	}
	_timer_lv_count = timer_lv_count;

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

	if (timer_lvs.size() != _timer_lv_count || timer_lvs[_timer_lv_count - 1] != 1)
	{
		fprintf(stderr, "Timer.timer_lvs needs %zu levels and the last one must be 1\n", (size_t)_timer_lv_count);
		return false;
	}
	// 各层格子数相乘不能溢出 uint32_t。
	uint64_t total = 1;
	for (size_t i = 0; i < timer_lvs.size(); ++i)
	{
		if (timer_lvs[i] > 0 && timer_lvs[i] <= 0xFFFFFFFFll)
		{
			total *= (uint64_t)timer_lvs[i];
		}
		if (timer_lvs[i] <= 0 || timer_lvs[i] > 0xFFFFFFFFll || total > 0xFFFFFFFFull)
		{
			fprintf(stderr, "Timer.timer_lvs[%zu]=%lld out of range\n", i, (long long)timer_lvs[i]);
			return false;
		}
	}

	uint32_t lv_tick = 1;
	for (size_t i = 0; i < _timer_lv_count; ++i)
	{
		_timer_levels.emplace_back();
		uint32_t slots = (uint32_t)timer_lvs[i];
		_timer_levels[i].Init(slots, lv_tick);
		lv_tick *= slots;
	}

	_tick_offset_ms = offset_ms;
	_last_tick_mono = std::chrono::steady_clock::now();
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
	bool queued = ::OI::Thread::ThreadPool::GetInstance().AddTask([]() {
		BaseTimerManager::GetInstance().Update();
		g_tick_queued.store(false);
	}, ::OI::Thread::PRIO_ENUM::PRIO_QUEUE);
	if (!queued)
	{
		// 队列满了这一跳不投。下次到点再投，错过的 tick 由 Update 按单调时钟补上。
		g_tick_queued.store(false);
	}
}

std::string BaseTimerManager::GenTraceLog()
{
	char buff[1024];
	snprintf(buff, sizeof(buff) - 1, 
		"BaseTimerManager::Update mono_ms=%lld wall=[%lld,%lld] tick_num=%lld",
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(_last_tick_mono.time_since_epoch()).count(),
		(long long)_cur_tv.tv_sec, (long long)_cur_tv.tv_usec,
		(long long)_cur_tick
	);
	return buff;
}

void BaseTimerManager::Update()
{
	int64_t ticks = 0;
	{
		::OI::Thread::MutexGuard lock(_wheel_mu);
		gettimeofday(&_cur_tv, 0);
		_cur_time = _cur_tv.tv_sec;
		localtime_r(&_cur_time, &_cur_tm);
		const int64_t delta_us = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - _last_tick_mono).count();
		if (delta_us <= 0)
		{
			return;
		}
		// 只取完整的 tick，余数留在单调时钟基点里，系统时间回拨不会暂停计时。
		ticks = (delta_us / 1000) / (int64_t)_tick_offset_ms;
	}

	// 锁里只推进时间轮。OnTimer 在锁外跑，回调里发网络、建目录不挡别的线程 AddTimer。
	for (int64_t i = 0; i < ticks; ++i)
	{
		TimerList due;
		{
			::OI::Thread::MutexGuard lock(_wheel_mu);
			run_tick(due);
		}
		for (TimerList::iterator it = due.begin(); it != due.end(); ++it)
		{
			run_timer(*it);
		}
	}
}

void BaseTimerManager::AddTimer(BaseTimer* pTimer, int millisecond)
{
	::OI::Thread::MutexGuard lock(_wheel_mu);
	pTimer->InitData(this, millisecond, TicksFromNow(millisecond));
	add_timer(pTimer);
}

int64_t BaseTimerManager::TicksFromNow(int millisecond) const
{
	int64_t elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - _last_tick_mono).count();
	if (elapsed_us < 0)
	{
		elapsed_us = 0;
	}
	const int64_t tick_us = (int64_t)_tick_offset_ms * 1000;
	const int64_t due_us = elapsed_us + (int64_t)millisecond * 1000;
	int64_t ticks = (due_us + tick_us - 1) / tick_us;
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

	if (pTimer->OnTimer())
	{
		::OI::Thread::MutexGuard lock(_wheel_mu);
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
			_timer_levels[i].RunTick(this, NULL);
		}
	}
}

void BaseTimerManager::try_run_timer(TimerList& due)
{
	//取出本tick到点的定时器
	TimerLevel& level = _timer_levels[0];
	level.RunTick(this, &due);
}

void BaseTimerManager::run_tick(TimerList& due)
{
	_last_tick_mono += std::chrono::milliseconds(_tick_offset_ms);
	++_cur_tick;

	try_run_timer(due);

	//第一层如果到尾,需要触发上级均匀
	if (_timer_levels[0].IsLevelEnd())
	{
		try_uniform();
	}
}
}}
