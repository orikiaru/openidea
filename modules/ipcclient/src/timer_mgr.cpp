#include "timer_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "oi_http.h"
#include "oi_log_client.h"
#include "oi_log.h"
#include "oi_registry.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "oi_rpcdata_jsonall.h"
#include <limits>
#include "oi_config.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "oi_poll.h"
#include "oi_net_session.h"
#include "oi_timer_manager.h"
#include <uv.h>
#include "test_board.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>



// 秒更新单独注册。一个单例不能同时有 OnTickUpdate 和 OnSecondUpdate。
class HoldSecondProbe
{
	DECLARE_SINGLETON(HoldSecondProbe)
	HoldSecondProbe();
	std::atomic<bool> m_bWatchUpdate;
	std::atomic<int32_t> m_iSecondUpdates;
	std::atomic<int32_t> m_iSecondActive;
	std::atomic<int32_t> m_iSecondOverlap;

  public:
	OI::Coro::Task OnSecondUpdate(time_t iNow);
	void WatchUpdate(bool bWatch);
	int32_t SecondUpdates() const
	{
		return m_iSecondUpdates.load(std::memory_order_relaxed);
	}
	int32_t SecondActive() const
	{
		return m_iSecondActive.load(std::memory_order_relaxed);
	}
	int32_t SecondOverlap() const
	{
		return m_iSecondOverlap.load(std::memory_order_relaxed);
	}
};

HoldSecondProbe::HoldSecondProbe()
    : m_bWatchUpdate(false), m_iSecondUpdates(0), m_iSecondActive(0), m_iSecondOverlap(0)
{
}

OI::Coro::Task HoldSecondProbe::OnSecondUpdate(time_t iNow)
{
	(void)iNow;
	if (!m_bWatchUpdate.load(std::memory_order_relaxed))
	{
		co_return;
	}
	const int32_t iPreviousActive = m_iSecondActive.fetch_add(1, std::memory_order_relaxed);
	if (iPreviousActive != 0)
	{
		m_iSecondOverlap.fetch_add(1, std::memory_order_relaxed);
	}
	m_iSecondUpdates.fetch_add(1, std::memory_order_relaxed);
	co_await OI::Coro::yield(0);
	m_iSecondActive.fetch_sub(1, std::memory_order_relaxed);
	co_return;
}

void HoldSecondProbe::WatchUpdate(bool bWatch)
{
	if (bWatch)
	{
		m_iSecondUpdates.store(0, std::memory_order_relaxed);
		m_iSecondOverlap.store(0, std::memory_order_relaxed);
		m_iSecondActive.store(0, std::memory_order_relaxed);
	}
	m_bWatchUpdate.store(bWatch, std::memory_order_relaxed);
}

struct TimerExceptionProbe
{
	uv_timer_t stWatchdog{};
	std::atomic<int> iExecuted{0};
	std::atomic<int> iDestroyed{0};
	std::atomic<bool> bClosed{false};
	std::atomic<bool> bPassed{false};
	std::atomic<int64_t> vTicks[5]{};
	uint64_t iDeadlineMilliseconds = 0;
};

struct TimerClosureProbe
{
	std::shared_ptr<TimerExceptionProbe> pState;
	explicit TimerClosureProbe(std::shared_ptr<TimerExceptionProbe> pState)
	    : pState(std::move(pState))
	{
	}
	~TimerClosureProbe()
	{
		++pState->iDestroyed;
	}
};

OI::Coro::TaskResult<int> TimerMgr::CheckTimerExceptions()
{
	TimerMgr::GetInstance().WatchUpdate(true);
	HoldSecondProbe::GetInstance().WatchUpdate(true);
	auto pState = std::make_shared<TimerExceptionProbe>();
	// 帧持有状态直到 uv_close 回调完成；冻结时间轮也不会阻止独立看门狗。
	OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		auto* pProbe = static_cast<TimerExceptionProbe*>(pArgument);
		auto* pLoop = OI::Net::Poll::GetInstance().Loop();
		if (uv_timer_init(pLoop, &pProbe->stWatchdog) != 0)
		{
			pProbe->bClosed = true;
			return;
		}
		pProbe->stWatchdog.data = pProbe;
		pProbe->iDeadlineMilliseconds = uv_now(pLoop) + 10000;
		const int iReturnCode = uv_timer_start(&pProbe->stWatchdog, [](uv_timer_t* pTimer)
		{
			auto* pProbeLocal = static_cast<TimerExceptionProbe*>(pTimer->data);
			const bool bComplete = pProbeLocal->iExecuted == 5 && pProbeLocal->iDestroyed == 5;
			if (!bComplete && uv_now(pTimer->loop) < pProbeLocal->iDeadlineMilliseconds)
			{
				return;
			}
			pProbeLocal->bPassed = bComplete;
			uv_timer_stop(pTimer);
			uv_close(reinterpret_cast<uv_handle_t*>(pTimer), [](uv_handle_t* pHandle)
			{
				static_cast<TimerExceptionProbe*>(pHandle->data)->bClosed = true;
			});
		}, 10, 10);
		if (iReturnCode != 0)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(&pProbe->stWatchdog), [](uv_handle_t* pHandle)
			{
				static_cast<TimerExceptionProbe*>(pHandle->data)->bClosed = true;
			});
		}
	}, pState.get());
	// 在时间轮回调内布置下一批，记录实际 tick，确保覆盖同批余下回调。
	OI::Timer::BaseTimerManager::GetInstance().AddTimer([pState]()
	{
		auto& stTimers = OI::Timer::BaseTimerManager::GetInstance();
		for (int iIndex = 0; iIndex < 5; ++iIndex)
		{
			auto pClosure = std::make_shared<TimerClosureProbe>(pState);
			stTimers.AddTimer([pClosure, iIndex]()
			{
				auto pProbe = pClosure->pState;
				pProbe->vTicks[iIndex] = OI::Timer::BaseTimerManager::GetInstance().GetCurTick();
				++pProbe->iExecuted;
				if (iIndex == 0)
				{
					throw std::runtime_error("hold timer std exception");
				}
				if (iIndex == 2)
				{
					throw 73;
				}
			}, iIndex == 4 ? stTimers.GetTickOffsetMS() * 3 : 0);
		}
	}, 0);
	while (!pState->bClosed.load())
	{
		co_await OI::Coro::yield(0);
	}
	const time_t iBeginSecond = OI::Timer::BaseTimerManager::GetInstance().GetTime();
	while (HoldSecondProbe::GetInstance().SecondUpdates() == 0 && OI::Timer::BaseTimerManager::GetInstance().GetTime() <= iBeginSecond + 2)
	{
		co_await OI::Coro::yield(OI::Timer::BaseTimerManager::GetInstance().GetTickOffsetMS());
	}
	TimerMgr::GetInstance().WatchUpdate(false);
	HoldSecondProbe::GetInstance().WatchUpdate(false);
	const int64_t iElapsedTicks = OI::Timer::BaseTimerManager::GetInstance().GetCurTick() - TimerMgr::GetInstance().WatchBeginTick();
	const int iTickOffsetMilliseconds = OI::Timer::BaseTimerManager::GetInstance().GetTickOffsetMS();
	int iWaitedMilliseconds = 0;
	while ((TimerMgr::GetInstance().TickActive() != 0 || HoldSecondProbe::GetInstance().SecondActive() != 0) && iWaitedMilliseconds < 1000)
	{
		co_await OI::Coro::yield(iTickOffsetMilliseconds);
		iWaitedMilliseconds += iTickOffsetMilliseconds;
	}
	const int32_t iTickUpdates = TimerMgr::GetInstance().TickUpdates();
	const int32_t iSecondUpdates = HoldSecondProbe::GetInstance().SecondUpdates();
	const int32_t iTickOverlap = TimerMgr::GetInstance().TickOverlap();
	const int32_t iSecondOverlap = HoldSecondProbe::GetInstance().SecondOverlap();
	const int32_t iTickActive = TimerMgr::GetInstance().TickActive();
	const int32_t iSecondActive = HoldSecondProbe::GetInstance().SecondActive();
	const int iSkipped = static_cast<int>(iElapsedTicks - iTickUpdates);
	int iBad = pState->bPassed ? 0 : 1;
	if (pState->vTicks[0] != pState->vTicks[1].load() || pState->vTicks[2] != pState->vTicks[3].load() || pState->vTicks[4] <= pState->vTicks[3].load())
	{
		++iBad;
	}
	const int iUpdateBad = (iTickUpdates > 0 && iSecondUpdates > 0 && iTickOverlap == 0 && iSecondOverlap == 0 && iTickActive == 0 && iSecondActive == 0 && iTickUpdates < iElapsedTicks) ? 0 : 1;
	LOG_TRACE("hold timer exceptions executed=%d destroyed=%d bad=%d", pState->iExecuted.load(), pState->iDestroyed.load(), iBad);
	LOG_TRACE("hold timer update tick=%d second=%d overlap=%d/%d skipped=%d bad=%d", iTickUpdates, iSecondUpdates, iTickOverlap, iSecondOverlap, iSkipped, iUpdateBad);
	iBad += iUpdateBad;
	fflush(stdout);
	co_return iBad;
}

TimerMgr::TimerMgr()
    : m_bWatchUpdate(false), m_iTickUpdates(0), m_iTickActive(0), m_iTickOverlap(0), m_iWatchBeginTick(0)
{
	HoldSecondProbe::GetInstance();
}

OI::Coro::Task TimerMgr::OnTickUpdate(uint64_t iNowMilliseconds)
{
	(void)iNowMilliseconds;
	if (!m_bWatchUpdate.load(std::memory_order_relaxed))
	{
		co_return;
	}
	const int32_t iPreviousActive = m_iTickActive.fetch_add(1, std::memory_order_relaxed);
	if (iPreviousActive != 0)
	{
		m_iTickOverlap.fetch_add(1, std::memory_order_relaxed);
	}
	m_iTickUpdates.fetch_add(1, std::memory_order_relaxed);
	// 跨过好几跳。没 co_return 之前，这一单例后续到点的跳直接跳过、不补。
	co_await OI::Coro::yield(OI::Timer::BaseTimerManager::GetInstance().GetTickOffsetMS() * 3);
	m_iTickActive.fetch_sub(1, std::memory_order_relaxed);
	co_return;
}

void TimerMgr::WatchUpdate(bool bWatch)
{
	if (bWatch)
	{
		m_iTickUpdates.store(0, std::memory_order_relaxed);
		m_iTickOverlap.store(0, std::memory_order_relaxed);
		m_iTickActive.store(0, std::memory_order_relaxed);
		m_iWatchBeginTick = OI::Timer::BaseTimerManager::GetInstance().GetCurTick();
	}
	m_bWatchUpdate.store(bWatch, std::memory_order_relaxed);
}

// 定时器回调抛异常后其余回调仍执行；跳更新和秒更新不重叠，挂起期间到点的跳跳过不补。
OI::Coro::TaskResult<int> TimerMgr::Run()
{
	co_return co_await CheckTimerExceptions();
}
