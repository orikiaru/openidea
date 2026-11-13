#include "schedule_mgr.h"
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


// 64 个协程不停 yield(0) 时，新协程仍能在 1.5 秒内跑到。
OI::Coro::TaskResult<int> ScheduleMgr::CheckSchedulerFairness()
{
	int iBadLocal = 0;
	std::atomic<bool> bOrdinaryDone{false};
	OI::Coro::TaskScope stBusyScope;
	const int64_t iFairnessBegin = ::OI::Timer::SteadyMilliseconds();
	const int64_t iFairnessLimit = ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	for (int iIndexLocal = 0; iIndexLocal < 64; ++iIndexLocal)
	{
		stBusyScope.Start([&bOrdinaryDone, iFairnessBegin, iFairnessLimit](OI::Coro::Token, OI::Coro::StopToken stStop) -> OI::Coro::Task
		{
			while (!bOrdinaryDone.load() && !stStop.StopRequested() && ::OI::Timer::SteadyMilliseconds() - iFairnessBegin < iFairnessLimit)
			{
				co_await OI::Coro::yield(0);
			}
		});
	}
	co_await OI::Coro::yield(OI::Timer::Milliseconds(50));
	OI::Coro::StartTask([&bOrdinaryDone](OI::Coro::Token) -> OI::Coro::Task
	{
		bOrdinaryDone.store(true);
		co_return;
	});
	while (!bOrdinaryDone.load())
	{
		co_await OI::Coro::yield(0);
	}
	if (HoldTest::WallMs(iFairnessBegin) > 1500)
	{
		++iBadLocal;
	}
	stBusyScope.RequestStop();
	co_await stBusyScope.Join();
	LOG_TRACE("hold scheduler fairness bad=%d", iBadLocal);
	co_return iBadLocal;
}

// 子任务异常跨挂起点传回父任务；作用域关闭后不再接受任务。
OI::Coro::TaskResult<int> ScheduleMgr::CheckTaskScope()
{
	int iBadLocal = 0;
	auto fnThrowing = [](OI::Coro::Token, OI::Coro::StopToken) -> OI::Coro::Task
	{
		co_await OI::Coro::yield(::OI::Timer::Milliseconds(10));
		throw std::runtime_error("scope regression");
	};
	OI::Coro::TaskScope stFailures;
	stFailures.Start(fnThrowing);
	bool bCaught = false;
	try
	{
		co_await stFailures.Join();
	}
	catch (const std::runtime_error&)
	{
		bCaught = true;
	}
	if (!bCaught)
	{
		++iBadLocal;
	}
	OI::Coro::TaskScope stScope;
	std::atomic<int> iFinishedLocal{0}, iStarted{0};
	for (int iIndexLocal = 0; iIndexLocal < 128; ++iIndexLocal)
	{
		stScope.Start([&iFinishedLocal, &iStarted](OI::Coro::Token, OI::Coro::StopToken stStop) -> OI::Coro::Task
		{
			++iStarted;
			while (!stStop.StopRequested())
			{
				co_await OI::Coro::yield(::OI::Timer::Milliseconds(10));
			}
			++iFinishedLocal;
		});
	}
	while (iStarted.load() != 128)
	{
		co_await OI::Coro::yield(::OI::Timer::Milliseconds(10));
	}
	stScope.RequestStop();
	co_await stScope.Join();
	if (iFinishedLocal.load() != 128)
	{
		++iBadLocal;
	}
	try
	{
		stScope.Start(fnThrowing);
		++iBadLocal;
	}
	catch (const std::logic_error&)
	{
	}
	LOG_TRACE("hold scope finished=%d caught=%d bad=%d", iFinishedLocal.load(), bCaught, iBadLocal);
	co_return iBadLocal;
}

// 真正的带捕获协程 lambda：临时闭包离开后、挂起恢复后仍访问捕获。
OI::Coro::TaskResult<int> ScheduleMgr::CheckCapturedTasks()
{
	std::vector<OI::Coro::TaskResult<int>> vTasks;
	for (int iIndexLocal = 0; iIndexLocal < 32; ++iIndexLocal)
	{
		vTasks.push_back(OI::Coro::StartTask([text = std::string(4096, 'a' + iIndexLocal % 26), iIndexLocal](OI::Coro::Token iToken) -> OI::Coro::TaskResult<int>
		{
			co_await OI::Coro::yield(::OI::Timer::Milliseconds(0));
			co_await OI::Coro::yield(::OI::Timer::Milliseconds(10));
			co_return (!iToken || text != std::string(4096, 'a' + iIndexLocal % 26)) ? 1 : 0;
		}));
	}
	int iBad = 0;
	for (const auto& stTask : vTasks)
	{
		iBad += co_await stTask;
	}
	LOG_TRACE("hold captured tasks done=%zu bad=%d", vTasks.size(), iBad);
	co_return iBad;
}

// 64 个协程不停 yield(0) 时，新协程仍能在 1.5 秒内跑到。
OI::Coro::TaskResult<int> ScheduleMgr::RunFairness()
{
	co_return co_await CheckSchedulerFairness();
}

// 子任务异常传到父任务。作用域停止后不再接受新任务。
OI::Coro::TaskResult<int> ScheduleMgr::RunScope()
{
	co_return co_await CheckTaskScope();
}

// 带捕获的协程 lambda 挂起恢复后，仍能读到捕获的内容。
OI::Coro::TaskResult<int> ScheduleMgr::RunCaptured()
{
	co_return co_await CheckCapturedTasks();
}
