#include <unistd.h>
#include <stdio.h>
#include "oi_framework_timer.h"
#include "oi_thread.h"
#include "oi_poll.h"
namespace OI
{
namespace Timer
{

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

int32_t BaseTimerManager::Init()
{
	const auto& stConfig = ::OI::Base::Config::GlobalConfig();
	uint32_t iOffsetMilliseconds = 50;
	if (stConfig.Contains("Timer.offset_ms") && !stConfig.GetAs("Timer.offset_ms", iOffsetMilliseconds))
	{
		fprintf(stderr, "configuration read failed: key=Timer.offset_ms function=%s file=%s line=%d code=%d\n",
		    __func__, __FILE__, __LINE__, OI_ERR_INVALID_ARGUMENT);
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (iOffsetMilliseconds == 0 || 1000 % iOffsetMilliseconds != 0)
	{
		fprintf(stderr, "Timer.offset_ms=%u must divide 1000\n", iOffsetMilliseconds);
		return OI_ERR_INVALID_ARGUMENT;
	}

	uint32_t iTimerLevelCount = 4;
	if (stConfig.Contains("Timer.timer_lv_count") && !stConfig.GetAs("Timer.timer_lv_count", iTimerLevelCount))
	{
		fprintf(stderr, "configuration read failed: key=Timer.timer_lv_count function=%s file=%s line=%d code=%d\n",
		    __func__, __FILE__, __LINE__, OI_ERR_INVALID_ARGUMENT);
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (iTimerLevelCount <= 2)
	{
		fprintf(stderr, "Timer.timer_lv_count=%u must be bigger than 2\n", iTimerLevelCount);
		return OI_ERR_INVALID_ARGUMENT;
	}
	m_iTimerLevelCount = iTimerLevelCount;

	std::vector<int64_t> vTimerLevels;
	if (!stConfig.GetArrayAs("Timer.timer_lvs", vTimerLevels))
	{
		if (stConfig.Contains("Timer.timer_lvs"))
		{
			fprintf(stderr, "configuration read failed: key=Timer.timer_lvs function=%s file=%s line=%d code=%d\n",
			    __func__, __FILE__, __LINE__, OI_ERR_INVALID_ARGUMENT);
			return OI_ERR_INVALID_ARGUMENT;
		}
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
		return OI_ERR_INVALID_ARGUMENT;
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
			return OI_ERR_INVALID_ARGUMENT;
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
	gettimeofday(&m_stCurrentTimeValue, 0); //记录第0帧的时间
	m_iCurrentTime = time(NULL);
	m_iDispatchedSecond = m_iCurrentTime;
	localtime_r(&m_iCurrentTime, &m_stCurrentCalendarTime);
	// Windows / Linux 都用 poll 循环上的 libuv timer 叫醒
	::OI::Net::Poll::GetInstance().ArmTick(m_iTickOffsetMilliseconds, OnLoopTick);
	return OI_OK;
}

void FrameworkTimer::ReleasePendingAfterJoin()
{
	auto& stManager = BaseTimerManager::GetInstance();
	TimerLevelVector vPending;
	{
		::OI::Thread::MutexGuard stLock(stManager.m_stWheelMutex);
		vPending.swap(stManager.m_vTimerLevels);
	}
	// 捕获对象的析构可以再次登记定时器，此时新登记会被拒绝。
}

void BaseTimerManager::OnSignal()
{
	// 处在 libuv 的 C 回调里，异常不能穿出去。单个回调的异常 Update 已经接住，这里兜分配失败；错过的 tick 下次按单调时钟补上。
	try
	{
		Update();
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
	}
}

std::string BaseTimerManager::GenTraceLog()
{
	::OI::Thread::MutexGuard stLock(m_stWheelMutex);
	char vBuffer[1024];
	snprintf(vBuffer, sizeof(vBuffer) - 1,
	    "BaseTimerManager::Update mono_ms=%lld wall=[%lld,%lld] tick_num=%lld",
	    (long long)std::chrono::duration_cast<std::chrono::milliseconds>(m_stLastTickMonotonic.time_since_epoch()).count(),
	    (long long)m_stCurrentTimeValue.tv_sec, (long long)m_stCurrentTimeValue.tv_usec,
	    (long long)m_iCurrentTick);
	return vBuffer;
}

void BaseTimerManager::Update()
{
	int64_t iTicks = 0;
	{
		::OI::Thread::MutexGuard stLock(m_stWheelMutex);
		if (m_vTimerLevels.empty() || !m_iTickOffsetMilliseconds)
		{
			return;
		}
		gettimeofday(&m_stCurrentTimeValue, 0);
		m_iCurrentTime = m_stCurrentTimeValue.tv_sec;
		localtime_r(&m_iCurrentTime, &m_stCurrentCalendarTime);
		const int64_t iDeltaUs = std::chrono::duration_cast<std::chrono::microseconds>(
		    std::chrono::steady_clock::now() - m_stLastTickMonotonic)
		                             .count();
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
		while (!vDue.empty())
		{
			RunDueTimer(vDue);
		}
	}
	if (iTicks > 0)
	{
		DispatchUpdates();
	}
}

void BaseTimerManager::DispatchUpdates()
{
	const uint64_t iNowMilliseconds = (uint64_t)m_stCurrentTimeValue.tv_sec * 1000 + (uint64_t)(m_stCurrentTimeValue.tv_usec / 1000);
	::OI::Base::SingletonUpdates::Instance().DispatchTick(iNowMilliseconds);
	if (m_iCurrentTime == m_iDispatchedSecond)
	{
		return;
	}
	m_iDispatchedSecond = m_iCurrentTime;
	::OI::Base::SingletonUpdates::Instance().DispatchSecond(m_iCurrentTime);
}

int32_t BaseTimerManager::RegisterTimer(TimerEntry stTimer, int iMillisecond)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("BaseTimerManager::AddTimer");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!stTimer.pTimer || iMillisecond <= 0)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	::OI::Thread::MutexGuard stLock(m_stWheelMutex);
	if (!m_iTickOffsetMilliseconds)
	{
		return OI_ERR_FAIL;
	}
	if (m_vTimerLevels.empty())
	{
		return OI_ERR_CANCELLED;
	}
	const int64_t iTicks = TicksFromNow(iMillisecond);
	stTimer.pTimer->InitData(iMillisecond, m_iCurrentTick + iTicks, m_stCurrentTimeValue, iTicks * m_iTickOffsetMilliseconds);
	FindSlot(*stTimer.pTimer).push_back(std::move(stTimer));
	return OI_OK;
}

int64_t BaseTimerManager::TicksFromNow(int iMillisecond) const
{
	int64_t iElapsedUs = std::chrono::duration_cast<std::chrono::microseconds>(
	    std::chrono::steady_clock::now() - m_stLastTickMonotonic)
	                         .count();
	if (iElapsedUs < 0)
	{
		iElapsedUs = 0;
	}
	const int64_t iTickUs = (int64_t)m_iTickOffsetMilliseconds * 1000;
	const int64_t iDueUs = iElapsedUs + (int64_t)iMillisecond * 1000;
	int64_t iTicks = (iDueUs + iTickUs - 1) / iTickUs;
	if (iTicks < 1)
	{
		iTicks = 1;
	}
	return iTicks;
}

TimerList& BaseTimerManager::FindSlot(const BaseTimer& stTimer)
{
	int64_t iOffset = stTimer.m_stData.m_iTriggerTick - m_iCurrentTick;
	// 新登记或续期的任务至少间隔一跳。级联时已经到期的任务直接进入本跳的待执行批次。
	if (iOffset <= 0)
	{
		iOffset = 1;
	}

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

		return stCurrentLevel.Slot((uint32_t)iLevelOffset);
	}
	return m_vTimerLevels.back().Slot(0);
}

void BaseTimerManager::RunDueTimer(TimerList& vDue)
{
	auto it = vDue.begin();
	auto* pTimer = it->pTimer;
	try
	{
		if (pTimer->OnTimer())
		{
			::OI::Thread::MutexGuard stLock(m_stWheelMutex);
			// 从回调完成时重新计时，错过的周期不在本次补帧中重放。
			const int64_t iTicks = TicksFromNow(pTimer->GetOffsetTime());
			pTimer->InitData(pTimer->GetOffsetTime(), m_iCurrentTick + iTicks, m_stCurrentTimeValue, iTicks * m_iTickOffsetMilliseconds);
			auto& vSlot = FindSlot(*pTimer);
			vSlot.splice(vSlot.end(), vDue, it);
			return;
		}
	}
	catch (...)
	{
		vDue.erase(it);
		::OI::Thread::ReportTaskError(std::current_exception());
		return;
	}
	vDue.erase(it);
}

void BaseTimerManager::try_uniform(TimerList& vDue)
{
	//尝试均匀其他层级定时器
	for (uint32_t iIndexLocal = 1; iIndexLocal < m_iTimerLevelCount; ++iIndexLocal)
	{
		if (!m_vTimerLevels[iIndexLocal - 1].IsLevelEnd())
		{
			break;
		}
		TimerList vSlot;
		m_vTimerLevels[iIndexLocal].RunTick(vSlot);
		while (!vSlot.empty())
		{
			auto* pTimer = vSlot.front().pTimer;
			if (pTimer->m_stData.m_iTriggerTick <= m_iCurrentTick)
			{
				vDue.splice(vDue.end(), vSlot, vSlot.begin());
			}
			else
			{
				auto& vTarget = FindSlot(*pTimer);
				vTarget.splice(vTarget.end(), vSlot, vSlot.begin());
			}
		}
	}
}

void BaseTimerManager::try_run_timer(TimerList& vDue)
{
	//取出本tick到点的定时器
	TimerLevel& stLevel = m_vTimerLevels[0];
	stLevel.RunTick(vDue);
}

void BaseTimerManager::run_tick(TimerList& vDue)
{
	m_stLastTickMonotonic += std::chrono::milliseconds(m_iTickOffsetMilliseconds);
	++m_iCurrentTick;

	try_run_timer(vDue);

	//第一层如果到尾,需要触发上级均匀
	if (m_vTimerLevels[0].IsLevelEnd())
	{
		try_uniform(vDue);
	}
}
}
}
