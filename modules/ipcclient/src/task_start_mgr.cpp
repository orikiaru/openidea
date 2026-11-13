#include "task_start_mgr.h"
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


static thread_local bool g_bInHotStartCallback = false;
OI::Coro::Task TaskStartMgr::HotStartChild(std::shared_ptr<std::atomic<int>> pStep, std::shared_ptr<std::atomic<bool>> pInCallback)
{
	if (pInCallback)
	{
		pInCallback->store(g_bInHotStartCallback);
	}
	pStep->store(1);
	co_await OI::Coro::yield(0);
	pStep->store(2);
}
OI::Coro::TaskResult<int> TaskStartMgr::CheckHotStart()
{
	int iBad = 0;
	auto pStep = std::make_shared<std::atomic<int>>(0);
	auto stTask = HotStartChild(pStep);
	// 返回前已经跑完第一段。挂起后多线程可能已被别的工人跑完，所以不要求恰好停在 1。
	const int iStepAtCreate = pStep->load();
	if (iStepAtCreate == 0)
	{
		++iBad;
	}
	co_await stTask;
	if (pStep->load() != 2)
	{
		++iBad;
	}
	// 网络回调不在协程里，新建的任务只入队，不在回调线程上跑。
	struct Probe
	{
		std::atomic<bool> bDone{false};
		std::optional<OI::Coro::Task> stTask;
		std::shared_ptr<std::atomic<int>> pStep = std::make_shared<std::atomic<int>>(0);
		std::shared_ptr<std::atomic<bool>> pInCallback = std::make_shared<std::atomic<bool>>(true);
	};
	auto pProbe = std::make_shared<Probe>();
	OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgument)
	{
		auto* pProbeLocal = static_cast<Probe*>(pArgument);
		g_bInHotStartCallback = true;
		pProbeLocal->stTask.emplace(TaskStartMgr::GetInstance().HotStartChild(pProbeLocal->pStep, pProbeLocal->pInCallback));
		g_bInHotStartCallback = false;
		pProbeLocal->bDone.store(true);
	}, pProbe.get());
	while (!pProbe->bDone.load())
	{
		co_await OI::Coro::yield(1);
	}
	co_await *pProbe->stTask;
	if (pProbe->pStep->load() != 2 || pProbe->pInCallback->load())
	{
		++iBad;
	}
	LOG_TRACE("hold hot start inline=%d queued=%d bad=%d", iStepAtCreate != 0 ? 1 : 0, pProbe->pInCallback->load() ? 0 : 1, iBad);
	fflush(stdout);
	co_return iBad;
}
// 仅用于回归统计；任务的共享所有权由 Task 自己管理。
OI::Coro::TaskResult<int> TaskStartMgr::SharedWaitersChild(std::shared_ptr<SharedWaitersState> pState)
{
	++pState->iRuns;
	co_await OI::Coro::yield(OI::Timer::Milliseconds(20));
	co_return 42;
}
OI::Coro::Task TaskStartMgr::SharedWaiter(OI::Coro::TaskResult<int> stTask, std::shared_ptr<SharedWaitersState> pState, int iIndex)
{
	const int iResult = co_await stTask;
	if (iResult != 42 || pState->vHits[iIndex].fetch_add(1) != 0)
	{
		++pState->iBad;
	}
	++pState->iAwakened;
}
OI::Coro::TaskResult<int> TaskStartMgr::CheckStartedTaskWaiters()
{
	int iBad = 0;
	for (const auto& iCount : {2, 8, 64, 256})
	{
		for (int iMode = 0; iMode < 3; ++iMode)
		{
			auto pState = std::make_shared<SharedWaitersState>(iCount);
			std::vector<OI::Coro::Task> vWaiters;
			{
				auto stTask = OI::Coro::StartTask([pState](OI::Coro::Token)
				{
					return TaskStartMgr::GetInstance().SharedWaitersChild(pState);
				});
				if (stTask.GetToken() == 0)
				{
					++pState->iBad;
				}
				if (iMode == 2 && (co_await stTask) != 42)
				{
					++pState->iBad;
				}
				for (int iIndex = 0; iIndex < iCount; ++iIndex)
				{
					// 混合场景：后一半等待者在任务自然完成后才启动。
					if (iMode == 1 && iIndex == iCount / 2 && (co_await stTask) != 42)
					{
						++pState->iBad;
					}
					vWaiters.push_back(SharedWaiter(stTask, pState, iIndex));
				}
			} // 创建者的 Task 析构，各等待者捕获的副本继续保活。
			for (const auto& stWaiter : vWaiters)
			{
				co_await stWaiter;
			}
			if (pState->iRuns.load() != 1 || pState->iAwakened.load() != iCount)
			{
				++pState->iBad;
			}
			for (const auto& iHits : pState->vHits)
			{
				if (iHits.load() != 1)
				{
					++pState->iBad;
				}
			}
			LOG_TRACE("hold started task mode=%d waiters=%d runs=%d awakened=%d bad=%d",
			    iMode, iCount, pState->iRuns.load(), pState->iAwakened.load(), pState->iBad.load());
			iBad += pState->iBad.load();
		}
	}
	co_return iBad;
}

// 已启动的共享任务只执行一次，每个等待者只被唤醒一次。
OI::Coro::TaskResult<int> TaskStartMgr::RunWaiters()
{
	co_return co_await CheckStartedTaskWaiters();
}

// 创建时先跑到第一次挂起。poll 回调里新建的任务只入队，不在回调里接着跑。
OI::Coro::TaskResult<int> TaskStartMgr::RunHot()
{
	co_return co_await CheckHotStart();
}
