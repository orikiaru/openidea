#include "task_share_mgr.h"
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


OI::Coro::TaskResult<std::string> TaskShareMgr::SharedResultChild(std::shared_ptr<std::atomic<int>> pRuns, bool bThrow)
{
	++*pRuns;
	co_await OI::Coro::yield(OI::Timer::Milliseconds(20));
	if (bThrow)
	{
		throw std::runtime_error("shared task failure");
	}
	co_return std::string(256, 's');
}
OI::Coro::Task TaskShareMgr::SharedVoidChild(std::shared_ptr<std::atomic<int>> pRuns)
{
	++*pRuns;
	co_await OI::Coro::yield(0);
}

OI::Coro::TaskResult<int> TaskShareMgr::SharedResultWaiter(OI::Coro::TaskResult<std::string> stTask, bool bThrow)
{
	int iBad = 0;
	// 第二遍是完成后再次等待，不能重新执行，也不能拿到被移空的字符串。
	for (int iRound = 0; iRound < 2; ++iRound)
	{
		try
		{
			auto sResult = co_await stTask;
			if (bThrow || sResult != std::string(256, 's'))
			{
				++iBad;
			}
		}
		catch (const std::runtime_error& stError)
		{
			if (!bThrow || std::string(stError.what()) != "shared task failure")
			{
				++iBad;
			}
		}
	}
	co_return iBad;
}
OI::Coro::Task TaskShareMgr::SharedVoidStarter(OI::Coro::Task stTask)
{
	stTask.Start();
	stTask.Start();
	co_await stTask;
	co_await stTask;
}

OI::Coro::TaskResult<int> TaskShareMgr::CheckForeignTaskStart()
{
#ifdef _SINGLE_THREAD_
	auto pRejected = std::make_shared<std::atomic<int>>(0);
	auto pRuns = std::make_shared<std::atomic<int>>(0);
	// 故意从非 poll 线程调用，用于验证拒绝；应用和后台工作不得这样启动协程。
	std::thread stForeign([pRejected, pRuns]()
	{
		try
		{
			auto stTask = OI::Coro::StartTask([pRuns](OI::Coro::Token)
			{
				++*pRuns;
				return TaskShareMgr::GetInstance().SharedVoidChild(pRuns);
			});
		}
		catch (const std::logic_error&)
		{
			++*pRejected;
		}
	});
	stForeign.join();
	const int iBad = pRejected->load() != 1 || pRuns->load() != 0;
	LOG_TRACE("hold foreign task st rejected=%d runs=%d bad=%d", pRejected->load(), pRuns->load(), iBad);
	co_return iBad;
#else
	LOG_TRACE("hold foreign task mt skipped=1 bad=0");
	co_return 0;
#endif
}

OI::Coro::TaskResult<int> TaskShareMgr::CheckSharedTasks()
{
	int iBad = 0;
	for (int iMode = 0; iMode < 2; ++iMode)
	{
		auto pRuns = std::make_shared<std::atomic<int>>(0);
		auto stTask = SharedResultChild(pRuns, iMode != 0);
		std::vector<OI::Coro::TaskResult<int>> vWaiters;
		for (int iIndex = 0; iIndex < 64; ++iIndex)
		{
			vWaiters.push_back(SharedResultWaiter(stTask, iMode != 0));
		}
		for (const auto& stWaiter : vWaiters)
		{
			iBad += co_await stWaiter;
		}
		if (pRuns->load() != 1)
		{
			++iBad;
		}
	}
	auto pRuns = std::make_shared<std::atomic<int>>(0);
	auto stTask = SharedVoidChild(pRuns);
	std::vector<OI::Coro::Task> vStarters;
	for (int iIndex = 0; iIndex < 64; ++iIndex)
	{
		vStarters.push_back(SharedVoidStarter(stTask));
	}
	for (const auto& stStarter : vStarters)
	{
		co_await stStarter;
	}
	if (pRuns->load() != 1)
	{
		++iBad;
	}
	// 等待器独立保活：创建等待器后移动/销毁原 Task 仍安全。
	auto pLifetimeRuns = std::make_shared<std::atomic<int>>(0);
	auto pLifetimeTask = std::make_unique<OI::Coro::TaskResult<std::string>>(SharedResultChild(pLifetimeRuns, false));
	auto stWaiter = pLifetimeTask->operator co_await();
	pLifetimeTask.reset();
	if ((co_await stWaiter) != std::string(256, 's') || pLifetimeRuns->load() != 1)
	{
		++iBad;
	}
	auto stSource = SharedVoidChild(pLifetimeRuns);
	auto stMoved = std::move(stSource);
	try
	{
		co_await stSource;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	try
	{
		stSource.Start();
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	co_await stMoved;
	auto stCopy = stMoved;
	stSource = stCopy;
	auto* pSelf = &stCopy;
	stCopy = *pSelf;
	stCopy = std::move(*pSelf);
	co_await stSource;
	// 协程里新建即运行：被替换掉的任务没人等，也照样跑完一次。
	auto pUnusedRuns = std::make_shared<std::atomic<int>>(0);
	auto stReplaced = SharedVoidChild(pUnusedRuns);
	stReplaced = std::move(stCopy);
	co_await stReplaced;
	if (pLifetimeRuns->load() != 2 || pUnusedRuns->load() != 1)
	{
		++iBad;
	}
	LOG_TRACE("hold shared tasks waiters=64 bad=%d", iBad);
	co_return iBad;
}

// 64 个等待者共享一个任务。完成后再次等待不重跑，拷贝、移动和替换都只执行一次。
OI::Coro::TaskResult<int> TaskShareMgr::Run()
{
	co_return co_await CheckSharedTasks();
}

// 单线程下从非 poll 线程 StartTask 被拒绝。多线程不测这条，直接记过。
OI::Coro::TaskResult<int> TaskShareMgr::RunForeign()
{
	co_return co_await CheckForeignTaskStart();
}
