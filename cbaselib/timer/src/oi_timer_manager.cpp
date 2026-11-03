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
	memset(&m_stCurrentTimeValue, 0, sizeof(m_stCurrentTimeValue));
	m_iCurrentTick = 0;
	m_iTickOffsetMilliseconds = 0;
}

bool BaseTimerManager::Init()
{
	uint32_t iOffsetMilliseconds = 50;
	TryGetConfigNoReturn("Timer.offset_ms", iOffsetMilliseconds);
	if (iOffsetMilliseconds == 0 || 1000 % iOffsetMilliseconds != 0)
	{
		fprintf(stderr, "Timer.offset_ms=%u must divide 1000\n", iOffsetMilliseconds);
		return false;
	}

	uint32_t iTimerLevelCount = 4;
	TryGetConfigNoReturn("Timer.timer_lv_count", iTimerLevelCount);
	if (iTimerLevelCount <= 2)
	{
		fprintf(stderr, "Timer.timer_lv_count=%u must be bigger than 2\n", iTimerLevelCount);
		return false;
	}
	m_iTimerLevelCount = iTimerLevelCount;

	std::vector<int64_t> vTimerLevels;
	if (!::OI::Base::Config::GlobalConfig().GetArrayAs("Timer.timer_lvs", vTimerLevels))
	{
		//没有就给一组默认数据,影响不大.
		std::vector<int64_t> vTmp;
		for (size_t iIndexLocal = 0; iIndexLocal < m_iTimerLevelCount; ++iIndexLocal)
		{
			vTmp.push_back(128);
		}
		vTimerLevels.swap(vTmp);
		vTimerLevels[m_iTimerLevelCount - 1] = 1;
	}

	if (vTimerLevels.size() != m_iTimerLevelCount || vTimerLevels[m_iTimerLevelCount - 1] != 1)
	{
		fprintf(stderr, "Timer.timer_lvs needs %zu levels and the last one must be 1\n", (size_t)m_iTimerLevelCount);
		return false;
	}
	// 各层格子数相乘不能溢出 uint32_t。
	uint64_t iTotal = 1;
	for (size_t iIndexLocal = 0; iIndexLocal < vTimerLevels.size(); ++iIndexLocal)
	{
		if (vTimerLevels[iIndexLocal] > 0 && vTimerLevels[iIndexLocal] <= 0xFFFFFFFFll)
		{
			iTotal *= (uint64_t)vTimerLevels[iIndexLocal];
		}
		if (vTimerLevels[iIndexLocal] <= 0 || vTimerLevels[iIndexLocal] > 0xFFFFFFFFll || iTotal > 0xFFFFFFFFull)
		{
			fprintf(stderr, "Timer.timer_lvs[%zu]=%lld out of range\n", iIndexLocal, (long long)vTimerLevels[iIndexLocal]);
			return false;
		}
	}

	uint32_t iLevelTick = 1;
	for (size_t iIndexLocal = 0; iIndexLocal < m_iTimerLevelCount; ++iIndexLocal)
	{
		m_vTimerLevels.emplace_back();
		uint32_t iSlots = (uint32_t)vTimerLevels[iIndexLocal];
		m_vTimerLevels[iIndexLocal].Init(iSlots, iLevelTick);
		iLevelTick *= iSlots;
	}

	m_iTickOffsetMilliseconds = iOffsetMilliseconds;
	m_stLastTickMonotonic = std::chrono::steady_clock::now();
	gettimeofday(&m_stCurrentTimeValue, 0);	 //记录第0帧的时间
	m_iCurrentTime = time(NULL);
	localtime_r(&m_iCurrentTime, &m_stCurrentCalendarTime);
	// Windows / Linux 都用 poll 循环上的 libuv timer 叫醒
	::OI::Net::Poll::GetInstance().ArmTick(m_iTickOffsetMilliseconds, OnLoopTick);
	return true;
}

void BaseTimerManager::OnSignal()
{
	bool bExpected = false;
	if (!g_tick_queued.compare_exchange_strong(bExpected, true))
	{
		return;
	}
	bool bQueued = ::OI::Thread::ThreadPool::GetInstance().AddTask([]() {
		BaseTimerManager::GetInstance().Update();
		g_tick_queued.store(false);
	}, ::OI::Thread::PRIO_ENUM::PRIO_QUEUE);
	if (!bQueued)
	{
		// 队列满了这一跳不投。下次到点再投，错过的 tick 由 Update 按单调时钟补上。
		g_tick_queued.store(false);
	}
}

std::string BaseTimerManager::GenTraceLog()
{
	char vBuffer[1024];
	snprintf(vBuffer, sizeof(vBuffer) - 1,
		"BaseTimerManager::Update mono_ms=%lld wall=[%lld,%lld] tick_num=%lld",
		(long long)std::chrono::duration_cast<std::chrono::milliseconds>(m_stLastTickMonotonic.time_since_epoch()).count(),
		(long long)m_stCurrentTimeValue.tv_sec, (long long)m_stCurrentTimeValue.tv_usec,
		(long long)m_iCurrentTick
	);
	return vBuffer;
}

void BaseTimerManager::Update()
{
	int64_t iTicks = 0;
	{
		::OI::Thread::MutexGuard stLock(m_stWheelMutex);
		gettimeofday(&m_stCurrentTimeValue, 0);
		m_iCurrentTime = m_stCurrentTimeValue.tv_sec;
		localtime_r(&m_iCurrentTime, &m_stCurrentCalendarTime);
		const int64_t iDeltaUs = std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - m_stLastTickMonotonic).count();
		if (iDeltaUs <= 0)
		{
			return;
		}
		// 只取完整的 tick，余数留在单调时钟基点里，系统时间回拨不会暂停计时。
		iTicks = (iDeltaUs / 1000) / (int64_t)m_iTickOffsetMilliseconds;
	}

	// 锁里只推进时间轮。OnTimer 在锁外跑，回调里发网络、建目录不挡别的线程 AddTimer。
	for (int64_t iIndexLocal = 0; iIndexLocal < iTicks; ++iIndexLocal)
	{
		TimerList vDue;
		{
			::OI::Thread::MutexGuard stLock(m_stWheelMutex);
			run_tick(vDue);
		}
		for (const auto& pTimer : vDue)
		{
			run_timer(pTimer);
		}
	}
}

void BaseTimerManager::AddTimer(BaseTimer* pTimerParam, int iMillisecondParam)
{
	::OI::Thread::MutexGuard stLock(m_stWheelMutex);
	pTimerParam->InitData(this, iMillisecondParam, TicksFromNow(iMillisecondParam));
	add_timer(pTimerParam);
}

int64_t BaseTimerManager::TicksFromNow(int iMillisecondParam) const
{
	int64_t iElapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - m_stLastTickMonotonic).count();
	if (iElapsedUs < 0)
	{
		iElapsedUs = 0;
	}
	const int64_t iTickUs = (int64_t)m_iTickOffsetMilliseconds * 1000;
	const int64_t iDueUs = iElapsedUs + (int64_t)iMillisecondParam * 1000;
	int64_t iTicks = (iDueUs + iTickUs - 1) / iTickUs;
	if (iTicks < 1)
	{
		iTicks = 1;
	}
	return iTicks;
}

void BaseTimerManager::add_timer(BaseTimer* pTimerParam)
{
	int64_t iOffset = pTimerParam->m_stData.m_iTriggerTick - m_iCurrentTick;
	// 粗层倒出来时可能已经过了点。放到下一跳，避免在均匀过程里再进回调。
	if (iOffset <= 0)
	{
		iOffset = 1;
	}

	bool bAdded = false;
	for (uint32_t iIndexLocal = 0; iIndexLocal < m_iTimerLevelCount - 1; ++iIndexLocal)
	{
		TimerLevel& stCurrentLevel = m_vTimerLevels[iIndexLocal];
		int64_t iLevelOffset = iOffset / (int64_t)stCurrentLevel.m_iLevelOffsetTick;
		assert(iLevelOffset >= 1);
		iLevelOffset -= 1;
		if (iLevelOffset >= (int64_t)stCurrentLevel.m_iMaxTick)
		{
			continue;
		}

		stCurrentLevel.AddTimer(this, pTimerParam, (uint32_t)iLevelOffset);
		bAdded = true;
		break;
	}

	if (!bAdded)
	{
		TimerLevel& stCurrentLevel = m_vTimerLevels[m_iTimerLevelCount - 1];
		stCurrentLevel.AddTimer(this, pTimerParam, 0);
	}
}

void BaseTimerManager::run_timer(BaseTimer* pTimerParam)
{
	assert(pTimerParam);

	if (pTimerParam->OnTimer())
	{
		::OI::Thread::MutexGuard stLock(m_stWheelMutex);
		// 周期定时器按逻辑 tick 续，停顿时后面的跳还能补上。
		int64_t iTicks = milling2tick(pTimerParam->GetOffsetTime());
		pTimerParam->InitData(this, pTimerParam->GetOffsetTime(), iTicks);
		add_timer(pTimerParam);
	}
}

void BaseTimerManager::try_uniform()
{
	//尝试均匀其他层级定时器
	for (uint32_t iIndexLocal = 1; iIndexLocal < m_iTimerLevelCount; ++iIndexLocal)
	{
		if (m_vTimerLevels[iIndexLocal - 1].IsLevelEnd())
		{
			//先执行tick,以便均匀后的定时器进入准确的offset
			m_vTimerLevels[iIndexLocal].RunTick(this, NULL);
		}
	}
}

void BaseTimerManager::try_run_timer(TimerList& vDueParam)
{
	//取出本tick到点的定时器
	TimerLevel& stLevel = m_vTimerLevels[0];
	stLevel.RunTick(this, &vDueParam);
}

void BaseTimerManager::run_tick(TimerList& vDueParam)
{
	m_stLastTickMonotonic += std::chrono::milliseconds(m_iTickOffsetMilliseconds);
	++m_iCurrentTick;

	try_run_timer(vDueParam);

	//第一层如果到尾,需要触发上级均匀
	if (m_vTimerLevels[0].IsLevelEnd())
	{
		try_uniform();
	}
}
}}
