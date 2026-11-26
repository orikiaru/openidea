#include "test_timer.h"
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
#include "oi_framework_timer.h"
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
#include <thread>
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

template <typename T>
concept TimerAccepts = requires(OI::Timer::BaseTimerManager& stTimers, T stValue) {
	stTimers.AddTimer(std::move(stValue), 1);
};
static_assert(!TimerAccepts<int*>);

struct TimerOnceState
{
	std::atomic<int32_t> iCalls{0};
	std::atomic<int32_t> iDestroyed{0};
};

struct TimerRepeatState
{
	std::atomic<int32_t> iCalls{0};
	std::atomic<int32_t> iThrows{0};
	std::atomic<int32_t> iStopped{0};
	std::atomic<int32_t> iExtended{0};
	std::atomic<int32_t> iSetZero{0};
	std::atomic<int32_t> iSetNegative{0};
	std::atomic<int32_t> vRemaining[3]{};
	std::atomic<int64_t> vTicks[3]{};
};

struct TimerBoundaryState
{
	std::atomic<int32_t> iCalls{0};
	std::atomic<int32_t> iBad{0};
	std::atomic<int32_t> iPhase{0};
};

struct FrameworkTimerProbeState
{
	std::atomic<int32_t> iInvalidCalls{0};
	std::atomic<int32_t> iValidCalls{0};
};

class FrameworkTimerProbe : public OI::Timer::BaseTimer
{
	std::shared_ptr<FrameworkTimerProbeState> m_pState;
	bool m_bRepeat;
	int32_t m_iCalls = 0;

  public:
	FrameworkTimerProbe(std::shared_ptr<FrameworkTimerProbeState> pState, bool bRepeat)
	    : m_pState(std::move(pState)), m_bRepeat(bRepeat)
	{
	}
	bool OnTimer() override
	{
		if (m_bRepeat)
		{
			++m_pState->iInvalidCalls;
			if (++m_iCalls == 1)
			{
				return OI::Timer::RESET_TIMER;
			}
			delete this;
			return OI::Timer::END_TIMER;
		}
		++m_pState->iValidCalls;
		delete this;
		return OI::Timer::END_TIMER;
	}
};

namespace OI::Timer
{
class TimerTestAccess
{
  public:
	static int32_t CheckUninitialized()
	{
		BaseTimerManager stManager;
		const char* pProbe = std::getenv("OI_TIMER_UNINIT_PROBE");
		const bool bCheckUpdate = !pProbe || std::strcmp(pProbe, "convert") != 0;
		const bool bCheckConvert = !pProbe || std::strcmp(pProbe, "update") != 0;
		const auto stBefore = stManager.GetCurTickTime();
		if (bCheckUpdate)
		{
			std::printf("timer uninitialized probe update\n");
			std::fflush(stdout);
			stManager.Update();
		}
		const auto stAfter = stManager.GetCurTickTime();
		if (bCheckConvert)
		{
			std::printf("timer uninitialized probe convert\n");
			std::fflush(stdout);
		}
		const uint32_t iTicks = bCheckConvert ? stManager.milling2tick(1) : 0;
		const bool bPassed = stManager.GetCurTick() == 0 && stAfter.tv_sec == stBefore.tv_sec &&
		    stAfter.tv_usec == stBefore.tv_usec && iTicks == 0;
		std::printf("hold timer uninitialized update=%d convert=%d tick=%lld ticks=%u unchanged=%d bad=%d\n",
		    bCheckUpdate, bCheckConvert, static_cast<long long>(stManager.GetCurTick()), iTicks,
		    stAfter.tv_sec == stBefore.tv_sec && stAfter.tv_usec == stBefore.tv_usec, bPassed ? 0 : 1);
		return bPassed ? 0 : 1;
	}
	static void Backdate(BaseTimerManager& stManager, int32_t iMilliseconds)
	{
		OI::Thread::MutexGuard stLock(stManager.m_stWheelMutex);
		stManager.m_stLastTickMonotonic -= std::chrono::milliseconds(iMilliseconds);
	}
	static void Register(BaseTimerManager& stManager, BaseTimer* pTimer, int32_t iTicks)
	{
		OI::Thread::MutexGuard stLock(stManager.m_stWheelMutex);
		const int32_t iMilliseconds = iTicks * stManager.m_iTickOffsetMilliseconds;
		pTimer->InitData(iMilliseconds, stManager.m_iCurrentTick + iTicks, stManager.m_stCurrentTimeValue, iMilliseconds);
		stManager.add_timer(pTimer);
	}
};
}

struct TimerBacklogState
{
	int32_t iRepeat = 0;
	int32_t iStop = 0;
	int32_t iChild = 0;
	int32_t iChildCode = OI_ERR_FAIL;
	int64_t iFirstTick = -1;
	int64_t iSecondTick = -1;
	int64_t iSlowMilliseconds = 0;
};

int32_t TestTimer::CheckBacklog()
{
	return OI::Net::Poll::GetInstance().StartListener([](void*) -> int32_t
	{
		auto& stTimer = OI::Timer::BaseTimerManager::GetInstance();
		auto pState = std::make_shared<TimerBacklogState>();
		const int32_t iRepeatCode = stTimer.AddTimer([pState]()
		{
			++pState->iRepeat;
			if (pState->iRepeat == 1)
			{
				auto& stLocalTimer = OI::Timer::BaseTimerManager::GetInstance();
				pState->iFirstTick = stLocalTimer.GetCurTick();
				pState->iChildCode = stLocalTimer.AddTimer([pState]()
				{
					++pState->iChild;
				}, 1);
				const auto stBegin = std::chrono::steady_clock::now();
				std::this_thread::sleep_for(std::chrono::milliseconds(60));
				pState->iSlowMilliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stBegin).count();
			}
			else if (pState->iRepeat == 2)
			{
				pState->iSecondTick = OI::Timer::BaseTimerManager::GetInstance().GetCurTick();
			}
		}, 1, 3);
		const int32_t iStopCode = stTimer.AddTimer([pState](OI::Timer::TimerContext& stContext)
		{
			++pState->iStop;
			stContext.Stop();
		}, 1, 3);
		OI::Timer::TimerTestAccess::Backdate(stTimer, 220);
		stTimer.Update();
		const int32_t iFirstCount = pState->iRepeat;
		const int32_t iFirstChild = pState->iChild;
		OI::Timer::TimerTestAccess::Backdate(stTimer, 100);
		stTimer.Update();
		const int32_t iSecondCount = pState->iRepeat;
		OI::Timer::TimerTestAccess::Backdate(stTimer, 100);
		stTimer.Update();
		const bool bPassed = iRepeatCode == OI_OK && iStopCode == OI_OK && pState->iChildCode == OI_OK && iFirstCount == 1 && iFirstChild == 0 && iSecondCount == 2 && pState->iRepeat == 3 && pState->iStop == 1 && pState->iChild == 1 && pState->iSecondTick > pState->iFirstTick && pState->iSlowMilliseconds >= 50;
		std::printf("timer backlog first=%d second=%d final=%d child_first=%d child=%d stop=%d ticks=%lld/%lld bad=%d\n", iFirstCount, iSecondCount, pState->iRepeat, iFirstChild, pState->iChild, pState->iStop, static_cast<long long>(pState->iFirstTick), static_cast<long long>(pState->iSecondTick), bPassed ? 0 : 1);
		std::fflush(stdout);
		return bPassed ? OI_OK : OI_ERR_FAIL;
	}, nullptr);
}

class BoundaryTimer : public OI::Timer::BaseTimer
{
	std::shared_ptr<TimerBoundaryState> m_pState;
	int32_t m_iRequestedTicks;
	int64_t m_iTargetTick = 0;

  public:
	BoundaryTimer(std::shared_ptr<TimerBoundaryState> pState, int32_t iRequestedTicks)
	    : m_pState(std::move(pState)), m_iRequestedTicks(iRequestedTicks)
	{
	}
	void SetTarget(int64_t iTargetTick)
	{
		m_iTargetTick = iTargetTick;
	}
	bool OnTimer() override
	{
		const int64_t iActualTick = OI::Timer::BaseTimerManager::GetInstance().GetCurTick();
		if (iActualTick != m_iTargetTick)
		{
			std::fprintf(stderr, "timer boundary requested_ticks=%d target=%lld actual=%lld\n", m_iRequestedTicks, static_cast<long long>(m_iTargetTick), static_cast<long long>(iActualTick));
			++m_pState->iBad;
		}
		++m_pState->iCalls;
		delete this;
		return OI::Timer::END_TIMER;
	}
};

struct TimerOnceLifetime
{
	std::shared_ptr<TimerOnceState> pState;
	~TimerOnceLifetime()
	{
		++pState->iDestroyed;
	}
};

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

OI::Coro::TaskResult<int> TestTimer::CheckTimerExceptions()
{
	const int32_t iUninitializedBad = OI::Timer::TimerTestAccess::CheckUninitialized();
	auto& stInitialized = OI::Timer::BaseTimerManager::GetInstance();
	const int32_t iOffset = stInitialized.GetTickOffsetMS();
	const int32_t iConversionBad = iOffset > 0 && stInitialized.milling2tick(iOffset) == 1 && stInitialized.milling2tick(iOffset + 1) == 2 ? 0 : 1;
	LOG_TRACE("hold timer conversion offset=%d bad=%d", iOffset, iConversionBad);
	auto pFrameworkProbe = std::make_shared<FrameworkTimerProbeState>();
	auto* pZeroTimer = new FrameworkTimerProbe(pFrameworkProbe, true);
	auto* pNegativeTimer = new FrameworkTimerProbe(pFrameworkProbe, true);
	auto* pValidTimer = new FrameworkTimerProbe(pFrameworkProbe, false);
	const int32_t iZeroFramework = OI::Timer::FrameworkTimer::AddTimer(pZeroTimer, 0);
	const int32_t iNegativeFramework = OI::Timer::FrameworkTimer::AddTimer(pNegativeTimer, -1);
	const int32_t iValidFramework = OI::Timer::FrameworkTimer::AddTimer(pValidTimer, OI::Timer::BaseTimerManager::GetInstance().GetTickOffsetMS());
	if (iZeroFramework != OI_OK)
	{
		delete pZeroTimer;
	}
	if (iNegativeFramework != OI_OK)
	{
		delete pNegativeTimer;
	}
	if (iValidFramework != OI_OK)
	{
		delete pValidTimer;
	}
	auto pBoundary = std::make_shared<TimerBoundaryState>();
	OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		auto* pHolder = static_cast<std::shared_ptr<TimerBoundaryState>*>(pArgument);
		auto pState = *pHolder;
		delete pHolder;
		auto& stTimers = OI::Timer::BaseTimerManager::GetInstance();
		const int32_t iCode = stTimers.AddTimer([pState](OI::Timer::TimerContext& stContext)
		{
			auto& stLocalTimers = OI::Timer::BaseTimerManager::GetInstance();
			const int64_t iStartTick = stLocalTimers.GetCurTick();
			const int32_t iPhase = static_cast<int32_t>(iStartTick % 4);
			if (iPhase > 1 || (pState->iPhase.load() & (1 << iPhase)) != 0)
			{
				return;
			}
			pState->iPhase.fetch_or(1 << iPhase);
			if (pState->iPhase.load() == 3)
			{
				stContext.Stop();
			}
			for (const int32_t iTicks : {3, 4, 5, 15, 16, 17, 63, 64, 65})
			{
				auto pTimer = std::make_unique<BoundaryTimer>(pState, iTicks);
				OI::Timer::TimerTestAccess::Register(stLocalTimers, pTimer.get(), iTicks);
				pTimer->SetTarget(iStartTick + iTicks);
				pTimer.release();
			}
		}, 1, 64);
		if (iCode != OI_OK)
		{
			++pState->iBad;
		}
	}, new std::shared_ptr<TimerBoundaryState>(pBoundary));
	const auto stDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (pBoundary->iCalls.load() < 18 && std::chrono::steady_clock::now() < stDeadline)
	{
		co_await OI::Coro::yield(50);
	}
	const int32_t iBoundaryBad = pBoundary->iBad.load() + (pBoundary->iCalls.load() != 18 || pBoundary->iPhase.load() != 3 ? 1 : 0);
	LOG_TRACE("hold timer boundaries phase=%d calls=%d bad=%d", pBoundary->iPhase.load(), pBoundary->iCalls.load(), iBoundaryBad);
	const int32_t iFrameworkBad = (iZeroFramework == OI_ERR_INVALID_ARGUMENT && iNegativeFramework == OI_ERR_INVALID_ARGUMENT && iValidFramework == OI_OK && pFrameworkProbe->iInvalidCalls == 0 && pFrameworkProbe->iValidCalls == 1) ? 0 : 1;
	LOG_TRACE("hold framework timer zero=%d negative=%d valid=%d invalid_calls=%d valid_calls=%d bad=%d", iZeroFramework, iNegativeFramework, iValidFramework, pFrameworkProbe->iInvalidCalls.load(), pFrameworkProbe->iValidCalls.load(), iFrameworkBad);
	TestTimer::GetInstance().WatchUpdate(true);
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
			}, iIndex == 4 ? stTimers.GetTickOffsetMS() * 3 : 1);
		}
	}, 1);
	while (!pState->bClosed.load())
	{
		co_await OI::Coro::yield(0);
	}
	const time_t iBeginSecond = OI::Timer::BaseTimerManager::GetInstance().GetTime();
	while (HoldSecondProbe::GetInstance().SecondUpdates() == 0 && OI::Timer::BaseTimerManager::GetInstance().GetTime() <= iBeginSecond + 2)
	{
		co_await OI::Coro::yield(OI::Timer::BaseTimerManager::GetInstance().GetTickOffsetMS());
	}
	TestTimer::GetInstance().WatchUpdate(false);
	HoldSecondProbe::GetInstance().WatchUpdate(false);
	const int64_t iElapsedTicks = OI::Timer::BaseTimerManager::GetInstance().GetCurTick() - TestTimer::GetInstance().WatchBeginTick();
	const int iTickOffsetMilliseconds = OI::Timer::BaseTimerManager::GetInstance().GetTickOffsetMS();
	int iWaitedMilliseconds = 0;
	while ((TestTimer::GetInstance().TickActive() != 0 || HoldSecondProbe::GetInstance().SecondActive() != 0) && iWaitedMilliseconds < 1000)
	{
		co_await OI::Coro::yield(iTickOffsetMilliseconds);
		iWaitedMilliseconds += iTickOffsetMilliseconds;
	}
	const int32_t iTickUpdates = TestTimer::GetInstance().TickUpdates();
	const int32_t iSecondUpdates = HoldSecondProbe::GetInstance().SecondUpdates();
	const int32_t iTickOverlap = TestTimer::GetInstance().TickOverlap();
	const int32_t iSecondOverlap = HoldSecondProbe::GetInstance().SecondOverlap();
	const int32_t iTickActive = TestTimer::GetInstance().TickActive();
	const int32_t iSecondActive = HoldSecondProbe::GetInstance().SecondActive();
	const int iSkipped = static_cast<int>(iElapsedTicks - iTickUpdates);
	auto pOnceState = std::make_shared<TimerOnceState>();
	auto& stTimers = OI::Timer::BaseTimerManager::GetInstance();
	auto pLifetime = std::make_unique<TimerOnceLifetime>();
	pLifetime->pState = pOnceState;
	const int32_t iFirst = stTimers.AddTimer([pOnceState, pLifetime = std::move(pLifetime)]()
	{
		if (pLifetime)
		{
			++pOnceState->iCalls;
		}
	}, 1);
	const int32_t iSecond = stTimers.AddTimer([pOnceState]()
	{
		++pOnceState->iCalls;
	}, 1);
	co_await OI::Coro::yield(stTimers.GetTickOffsetMS() * 3);
	const int32_t iOnceBad = (iFirst == OI_OK && iSecond == OI_OK && pOnceState->iCalls == 2 && pOnceState->iDestroyed == 1) ? 0 : 1;
	LOG_TRACE("hold timer once calls=%d destroyed=%d bad=%d", pOnceState->iCalls.load(), pOnceState->iDestroyed.load(), iOnceBad);
	auto pRepeat = std::make_shared<TimerRepeatState>();
	const int32_t iInterval = stTimers.GetTickOffsetMS() * 2;
	const int32_t iRepeat = stTimers.AddTimer([pRepeat]()
	{
		const int32_t iIndex = pRepeat->iCalls.fetch_add(1);
		if (iIndex < 3)
		{
			pRepeat->vTicks[iIndex] = OI::Timer::BaseTimerManager::GetInstance().GetCurTick();
		}
	}, iInterval, 3);
	auto pThrowState = std::make_shared<TimerOnceState>();
	auto pThrowLifetime = std::make_unique<TimerOnceLifetime>();
	pThrowLifetime->pState = pThrowState;
	const int32_t iThrow = stTimers.AddTimer([pRepeat, pThrowLifetime = std::move(pThrowLifetime)]()
	{
		if (pThrowLifetime)
		{
			++pRepeat->iThrows;
		}
		throw std::runtime_error("hold repeat timer exception");
	}, iInterval, 3);
	const int32_t iStop = stTimers.AddTimer([pRepeat](OI::Timer::TimerContext& stContext)
	{
		++pRepeat->iStopped;
		stContext.Stop();
	}, iInterval, 3);
	const int32_t iExtend = stTimers.AddTimer([pRepeat](OI::Timer::TimerContext& stContext)
	{
		const int32_t iIndex = pRepeat->iExtended.fetch_add(1);
		if (iIndex < 3)
		{
			pRepeat->vRemaining[iIndex] = stContext.Remaining();
		}
		if (iIndex == 0)
		{
			stContext.SetRemaining(2);
		}
	}, iInterval, 1);
	const int32_t iSetZero = stTimers.AddTimer([pRepeat](OI::Timer::TimerContext& stContext)
	{
		++pRepeat->iSetZero;
		stContext.SetRemaining(0);
	}, iInterval, 3);
	const int32_t iSetNegative = stTimers.AddTimer([pRepeat](OI::Timer::TimerContext& stContext)
	{
		++pRepeat->iSetNegative;
		stContext.SetRemaining(-1);
	}, iInterval, 3);
	const int32_t iZeroCount = stTimers.AddTimer([]()
	{
	}, iInterval, 0);
	const int32_t iNegativeCount = stTimers.AddTimer([]()
	{
	}, iInterval, -1);
	const int32_t iZeroInterval = stTimers.AddTimer([]()
	{
	}, 0, 1);
	const int32_t iNegativeInterval = stTimers.AddTimer([]()
	{
	}, -1, 1);
	co_await OI::Coro::yield(iInterval * 6);
	const int32_t iRepeatBad = (iRepeat == OI_OK && iThrow == OI_OK && iZeroCount == OI_ERR_INVALID_ARGUMENT && iNegativeCount == OI_ERR_INVALID_ARGUMENT && iZeroInterval == OI_ERR_INVALID_ARGUMENT && iNegativeInterval == OI_ERR_INVALID_ARGUMENT && pRepeat->iCalls == 3 && pRepeat->iThrows == 1 && pThrowState->iDestroyed == 1 && pRepeat->vTicks[1] >= pRepeat->vTicks[0].load() + 2 && pRepeat->vTicks[2] >= pRepeat->vTicks[1].load() + 2) ? 0 : 1;
	LOG_TRACE("hold timer repeat calls=%d throws=%d zero_count=%d negative_count=%d zero_interval=%d negative_interval=%d bad=%d", pRepeat->iCalls.load(), pRepeat->iThrows.load(), iZeroCount, iNegativeCount, iZeroInterval, iNegativeInterval, iRepeatBad);
	const int32_t iContextBad = (iStop == OI_OK && iExtend == OI_OK && iSetZero == OI_OK && iSetNegative == OI_OK && pRepeat->iStopped == 1 && pRepeat->iExtended == 3 && pRepeat->iSetZero == 1 && pRepeat->iSetNegative == 1 && pRepeat->vRemaining[0] == 0 && pRepeat->vRemaining[1] == 1 && pRepeat->vRemaining[2] == 0) ? 0 : 1;
	LOG_TRACE("hold timer context stopped=%d extended=%d remaining=%d/%d/%d zero=%d negative=%d bad=%d", pRepeat->iStopped.load(), pRepeat->iExtended.load(), pRepeat->vRemaining[0].load(), pRepeat->vRemaining[1].load(), pRepeat->vRemaining[2].load(), pRepeat->iSetZero.load(), pRepeat->iSetNegative.load(), iContextBad);
	int iBad = (pState->bPassed ? 0 : 1) + iOnceBad + iBoundaryBad + iFrameworkBad + iUninitializedBad + iConversionBad;
	iBad += iRepeatBad + iContextBad;
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

TestTimer::TestTimer()
    : m_bWatchUpdate(false), m_iTickUpdates(0), m_iTickActive(0), m_iTickOverlap(0), m_iWatchBeginTick(0)
{
	HoldSecondProbe::GetInstance();
}

OI::Coro::Task TestTimer::OnTickUpdate(uint64_t iNowMilliseconds)
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

void TestTimer::WatchUpdate(bool bWatch)
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
OI::Coro::TaskResult<int> TestTimer::Run()
{
	co_return co_await CheckTimerExceptions();
}
