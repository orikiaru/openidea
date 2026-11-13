#include "coro_mgr.h"
#include "oi_app.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_thread.h"
#include <chrono>
#include <cstdio>
#include <cstring>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#ifdef _MSC_VER
#pragma comment(lib, "psapi.lib")
#endif
#endif

namespace
{
long ReadRssKb()
{
#ifdef _WIN32
	PROCESS_MEMORY_COUNTERS stMemory = {};
	stMemory.cb = sizeof(stMemory);
	if (!GetProcessMemoryInfo(GetCurrentProcess(), &stMemory, sizeof(stMemory)))
	{
		return -1;
	}
	return static_cast<long>(stMemory.WorkingSetSize / 1024ULL);
#else
	FILE* pFile = std::fopen("/proc/self/status", "r");
	if (!pFile)
	{
		return -1;
	}
	char sLine[256];
	long iRssKb = -1;
	while (std::fgets(sLine, sizeof(sLine), pFile))
	{
		if (std::strncmp(sLine, "VmRSS:", 6) == 0)
		{
			std::sscanf(sLine + 6, "%ld", &iRssKb);
			break;
		}
	}
	std::fclose(pFile);
	return iRssKb;
#endif
}

struct SuspendGate
{
	bool await_ready() const noexcept
	{
		return false;
	}
	void await_suspend(std::coroutine_handle<> stHandle) const
	{
		CoroMgr::GetInstance().Hold(stHandle);
	}
	void await_resume() const noexcept
	{
	}
};

int64_t NsOf(std::chrono::steady_clock::time_point stBegin, std::chrono::steady_clock::time_point stEnd)
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(stEnd - stBegin).count();
}

int64_t PerItemNs(std::chrono::steady_clock::time_point stBegin, std::chrono::steady_clock::time_point stEnd, int32_t iCount)
{
	if (iCount < 1)
	{
		return 0;
	}
	return NsOf(stBegin, stEnd) / iCount;
}

int64_t RateOf(int64_t iPerItemNs)
{
	if (iPerItemNs <= 0)
	{
		return 0;
	}
	return 1000000000LL / iPerItemNs;
}
}

CoroMgr::CoroMgr()
    : m_bEnabled(false), m_iLive(100000), m_iChurn(1000000), m_iWakeDone(0), m_iChurnDone(0)
{
}

void CoroMgr::Load()
{
	TryGetConfig("Test.coro", m_bEnabled);
	TryGetConfig("TestCoro.live", m_iLive);
	TryGetConfig("TestCoro.churn", m_iChurn);
	if (m_iLive < 1)
	{
		m_iLive = 1;
	}
	if (m_iChurn < 1)
	{
		m_iChurn = 1;
	}
}

void CoroMgr::Start()
{
	if (!m_bEnabled)
	{
		return;
	}
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return CoroMgr::GetInstance().Run();
	});
}

void CoroMgr::Hold(std::coroutine_handle<> stHandle)
{
	OI::Thread::MutexGuard stGuard(m_stHeldMutex);
	m_vHeld.push_back(stHandle);
}

int32_t CoroMgr::HeldCount()
{
	OI::Thread::MutexGuard stGuard(m_stHeldMutex);
	return static_cast<int32_t>(m_vHeld.size());
}

std::vector<std::coroutine_handle<>> CoroMgr::CopyHeld()
{
	OI::Thread::MutexGuard stGuard(m_stHeldMutex);
	return m_vHeld;
}

void CoroMgr::NoteWake()
{
	m_iWakeDone.fetch_add(1, std::memory_order_relaxed);
}

void CoroMgr::NoteChurn()
{
	m_iChurnDone.fetch_add(1, std::memory_order_relaxed);
}

int32_t CoroMgr::WakeDone() const
{
	return m_iWakeDone.load(std::memory_order_relaxed);
}

int32_t CoroMgr::ChurnDone() const
{
	return m_iChurnDone.load(std::memory_order_relaxed);
}

OI::Coro::Task CoroMgr::Run()
{
	{
		OI::Thread::MutexGuard stGuard(m_stHeldMutex);
		m_vHeld.clear();
	}
	m_iWakeDone.store(0, std::memory_order_relaxed);
	m_iChurnDone.store(0, std::memory_order_relaxed);

	const long iRssBaseKb = ReadRssKb();
	// 存活段：从第一条 StartTask 到 10 万条都停在 SuspendGate。多线程下入队和工人执行叠在一起，不再拆开报。
	const auto stLiveBegin = std::chrono::steady_clock::now();
	for (int32_t iIndex = 0; iIndex < m_iLive; ++iIndex)
	{
		OI::Coro::StartTask([](OI::Coro::Token) -> OI::Coro::Task
		{
			co_await SuspendGate{};
			CoroMgr::GetInstance().NoteWake();
			co_return;
		});
	}
	while (HeldCount() < m_iLive)
	{
		co_await OI::Coro::yield(0);
	}
	const auto stLiveEnd = std::chrono::steady_clock::now();
	const long iRssLiveKb = ReadRssKb();

	// 唤醒段：Schedule 全部挂起句柄，等到每条 NoteWake 后结束。
	const auto stWakeBegin = std::chrono::steady_clock::now();
	const std::vector<std::coroutine_handle<>> vWake = CopyHeld();
	for (std::coroutine_handle<> stHandle : vWake)
	{
		OI::Coro::Schedule(stHandle);
	}
	while (WakeDone() < m_iLive)
	{
		co_await OI::Coro::yield(0);
	}
	const auto stWakeEnd = std::chrono::steady_clock::now();

	// 空任务段：从第一条 StartTask 到 100 万条都跑完。同样只报端到端，不拆入队和执行。
	const auto stChurnBegin = std::chrono::steady_clock::now();
	for (int32_t iIndex = 0; iIndex < m_iChurn; ++iIndex)
	{
		OI::Coro::StartTask([](OI::Coro::Token) -> OI::Coro::Task
		{
			CoroMgr::GetInstance().NoteChurn();
			co_return;
		});
	}
	while (ChurnDone() < m_iChurn)
	{
		co_await OI::Coro::yield(0);
	}
	const auto stChurnEnd = std::chrono::steady_clock::now();

	const long iBytes = iRssLiveKb < 0 || iRssBaseKb < 0 ? -1 : (iRssLiveKb - iRssBaseKb) * 1024L / m_iLive;
	const int32_t iWorkers = ::OI::Thread::ThreadPool::GetInstance().stThreadData.iThreadCount;
	const int64_t iLiveNs = PerItemNs(stLiveBegin, stLiveEnd, m_iLive);
	const int64_t iWakeNs = PerItemNs(stWakeBegin, stWakeEnd, m_iLive);
	const int64_t iChurnNs = PerItemNs(stChurnBegin, stChurnEnd, m_iChurn);
	LOG_TRACE("coro live=%d churn=%d workers=%d rss_base_kb=%ld rss_live_kb=%ld bytes_per=%ld live_ns=%lld live_rate=%lld wake_ns=%lld wake_rate=%lld churn_ns=%lld churn_rate=%lld bad=0",
	    m_iLive, m_iChurn, iWorkers, iRssBaseKb, iRssLiveKb, iBytes, static_cast<long long>(iLiveNs), static_cast<long long>(RateOf(iLiveNs)),
	    static_cast<long long>(iWakeNs), static_cast<long long>(RateOf(iWakeNs)), static_cast<long long>(iChurnNs),
	    static_cast<long long>(RateOf(iChurnNs)));
	::OI::Thread::ThreadPool::GetInstance().RequestStop(0);
	co_return;
}
