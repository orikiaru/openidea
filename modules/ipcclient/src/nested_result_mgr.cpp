#include "nested_result_mgr.h"
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


OI::Coro::TaskResult<int> NestedResultMgr::ResultStressChild(int iValue)
{
	if (iValue & 1)
	{
		co_await OI::Coro::yield(0);
	}
	if (iValue % 8 == 2 || iValue % 8 == 3)
	{
		throw std::runtime_error("hold nested result exception");
	}
	if (iValue % 8 == 4 || iValue % 8 == 5)
	{
		throw iValue;
	}
	co_return iValue;
}

OI::Coro::TaskResult<int> NestedResultMgr::ResultStressParent(int iValue)
{
	co_return (co_await ResultStressChild(iValue)) + 1;
}

OI::Coro::TaskResult<NestedResultMgr::NestedCounts> NestedResultMgr::NestedResultsWorker(int iWorker)
{
	NestedCounts stCounts;
	for (int iIndex = 0; iIndex < 512; ++iIndex)
	{
		const int iValue = iWorker * 512 + iIndex;
		try
		{
			const int iResult = co_await NestedResultMgr::GetInstance().ResultStressParent(iValue);
			if (iResult != iValue + 1 || (iValue % 8 >= 2 && iValue % 8 <= 5))
			{
				++stCounts.iBad;
			}
		}
		catch (const std::runtime_error& stError)
		{
			++stCounts.iCaught;
			if ((iValue % 8 != 2 && iValue % 8 != 3) || std::strcmp(stError.what(), "hold nested result exception") != 0)
			{
				++stCounts.iBad;
			}
		}
		catch (int iError)
		{
			++stCounts.iCaught;
			if ((iValue % 8 != 4 && iValue % 8 != 5) || iError != iValue)
			{
				++stCounts.iBad;
			}
		}
		catch (...)
		{
			++stCounts.iBad;
		}
		++stCounts.iCompleted;
	}
	co_return stCounts;
}

OI::Coro::TaskResult<int> NestedResultMgr::CheckNestedResults()
{
	co_await OI::Coro::WhenAll(std::vector<OI::Coro::Task>{});
	const std::vector<int> vEmpty = co_await OI::Coro::WhenAll(std::vector<OI::Coro::TaskResult<int>>{});
	std::vector<OI::Coro::TaskResult<NestedCounts>> vWorkers;
	for (int iWorker = 0; iWorker < 64; ++iWorker)
	{
		vWorkers.push_back(NestedResultsWorker(iWorker));
	}
	const std::vector<NestedCounts> vCounts = co_await OI::Coro::WhenAll(std::move(vWorkers));
	NestedCounts stTotal;
	if (!vEmpty.empty() || vCounts.size() != 64)
	{
		++stTotal.iBad;
	}
	for (const auto& stCounts : vCounts)
	{
		stTotal.iCompleted += stCounts.iCompleted;
		stTotal.iCaught += stCounts.iCaught;
		stTotal.iBad += stCounts.iBad;
	}
	if (stTotal.iCompleted != 32768 || stTotal.iCaught != 16384)
	{
		++stTotal.iBad;
	}
	LOG_TRACE("hold nested results completed=%d caught=%d bad=%d", stTotal.iCompleted, stTotal.iCaught, stTotal.iBad);
	fflush(stdout);
	co_return stTotal.iBad;
}

// 嵌套 TaskResult：成功和抛异常的子任务都收齐，完成数和捕获数对得上。
OI::Coro::TaskResult<int> NestedResultMgr::Run()
{
	co_return co_await CheckNestedResults();
}
