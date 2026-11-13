#include "registry_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "zone_service.h"
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

OI::Coro::TaskResult<bool> RegistryMgr::RegistryFaultHandshake(std::string sPath, std::string sCommand)
{
	{
		std::ofstream stFile(sPath + ".request", std::ios::trunc);
		stFile << sCommand;
		if (!stFile)
		{
			co_return false;
		}
	}
	const int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 5000;
	while (OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
	{
		std::ifstream stFile(sPath + ".ack");
		std::string sAck;
		stFile >> sAck;
		if (sAck == sCommand)
		{
			co_return true;
		}
		co_await OI::Coro::yield(10);
	}
	co_return false;
}

OI::Coro::TaskResult<int> RegistryMgr::CheckRegistryBoundsAndBudget()
{
	std::string sFaultPath;
	TryGetConfig("Regression.registry_fault_path", sFaultPath);
	if (sFaultPath.empty())
	{
		co_return 1;
	}
	int iBad = 0;
	auto& stRegistry = OI::Base::RegistryClient::GetInstance();
	if (!(co_await RegistryFaultHandshake(sFaultPath, "pause")))
	{
		co_return 1;
	}
	// 暂停期间，已在缓存里的 gameserver 不访问注册表。
	int iPauseCachedBad = 0;
	static ZoneService stGames("gameserver");
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 831;
	const std::vector<int32_t> vManyZones{1001, 1002, 1001};
	const auto vMany = co_await stGames.CallMany<OI::RpcData::HoldRes>(vManyZones, stRequest, OI::Timer::Seconds(3));
	int iManyOk = 0;
	const int iManyCount = static_cast<int>(vMany.size());
	if (iManyCount == static_cast<int>(vManyZones.size()))
	{
		for (const auto& stResult : vMany)
		{
			if (stResult.GetRet() == OI_OK)
			{
				++iManyOk;
			}
		}
	}
	if (iManyOk != static_cast<int>(vManyZones.size()))
	{
		++iPauseCachedBad;
	}
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -831;
	const int32_t iCallCode = co_await stGames.Call(1001, stRequest, stOutput, OI::Timer::Seconds(3));
	if (iCallCode != OI_OK || stOutput.iMark != 831)
	{
		++iPauseCachedBad;
	}
	LOG_TRACE("hold registry pause cached many=%d/%d call=%d mark=%d bad=%d",
	    iManyOk, iManyCount, iCallCode, stOutput.iMark, iPauseCachedBad);
	fflush(stdout);
	iBad += iPauseCachedBad;
	// 从没拉到过的名字：缓存未命中，短预算超时，输出保持原值。
	int iPauseMissingBad = 0;
	std::vector<OI::Base::ServiceInstance> vMissing;
	const bool bMissingKnown = stRegistry.Snapshot("missing_paused", vMissing);
	if (bMissingKnown)
	{
		++iPauseMissingBad;
	}
	OI::RpcData::HoldReq stMissingRequest;
	OI::RpcData::HoldRes stMissingOutput;
	stMissingOutput.iMark = -731;
	const int64_t iMissingStartMilliseconds = OI::Timer::SteadyMilliseconds();
	const int32_t iMissingCode = co_await OI::Rpc::RpcCall(std::string("missing_paused.1"), stMissingRequest, stMissingOutput, OI::Timer::Milliseconds(300));
	const long long iMissingElapsedMilliseconds = OI::Timer::SteadyMilliseconds() - iMissingStartMilliseconds;
	if (iMissingCode != OI_ERR_TIMEOUT || iMissingElapsedMilliseconds >= 450 || stMissingOutput.iMark != -731)
	{
		++iPauseMissingBad;
	}
	LOG_TRACE("hold registry pause missing snapshot=%d code=%d elapsed_ms=%lld mark=%d bad=%d",
	    bMissingKnown ? 1 : 0, iMissingCode, iMissingElapsedMilliseconds, stMissingOutput.iMark, iPauseMissingBad);
	fflush(stdout);
	iBad += iPauseMissingBad;
	if (!(co_await RegistryFaultHandshake(sFaultPath, "resume")))
	{
		co_return iBad + 1;
	}
	// 恢复后后台拉取写入空名单。空名单在 Resolve 里记 Offline，RouteError 默认返回 OI_ERR_CONNECT。
	int iResumeBad = 0;
	bool bResumedKnown = false;
	std::vector<OI::Base::ServiceInstance> vResumed;
	const int64_t iResumeDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3500;
	while (OI::Timer::SteadyMilliseconds() < iResumeDeadlineMilliseconds)
	{
		bResumedKnown = stRegistry.Snapshot("missing_paused", vResumed);
		if (bResumedKnown)
		{
			break;
		}
		co_await OI::Coro::yield(10);
	}
	const bool bResumedEmpty = bResumedKnown && vResumed.empty();
	if (!bResumedKnown || !bResumedEmpty)
	{
		++iResumeBad;
	}
	stMissingOutput.iMark = -732;
	const int32_t iOfflineCode = co_await OI::Rpc::RpcCall(std::string("missing_paused.1"), stMissingRequest, stMissingOutput, OI::Timer::Milliseconds(1000));
	if (iOfflineCode != OI_ERR_CONNECT)
	{
		++iResumeBad;
	}
	LOG_TRACE("hold registry resume missing snapshot=%d empty=%d code=%d bad=%d",
	    bResumedKnown ? 1 : 0, bResumedEmpty ? 1 : 0, iOfflineCode, iResumeBad);
	fflush(stdout);
	iBad += iResumeBad;
	// 批量里夹非法分区，结果条数不变，每条失败。
	int iSlotsBad = 0;
	const auto vInvalid = co_await stGames.CallMany<OI::RpcData::HoldRes>({1001, -1, 1002}, stRequest, OI::Timer::Seconds(3));
	if (vInvalid.size() != 3)
	{
		++iSlotsBad;
	}
	for (const auto& stResult : vInvalid)
	{
		if (stResult.GetRet() != OI_ERR_FAIL)
		{
			++iSlotsBad;
		}
	}
	LOG_TRACE("hold batch failure slots bad=%d", iSlotsBad);
	fflush(stdout);
	iBad += iSlotsBad;
	if (!(co_await RegistryFaultHandshake(sFaultPath, "budget")))
	{
		co_return iBad + 1;
	}
	// 注册表短暂停后再恢复。未缓存名字等后台拉取；已缓存调用按自己的预算完成。
	int iBudgetBad = 0;
	OI::RpcData::HoldReq stBudgetRequest;
	stBudgetRequest.iDelayMilliseconds = 900;
	stBudgetRequest.iMark = 841;
	OI::RpcData::HoldRes stBudgetOutput;
	stBudgetOutput.iMark = -841;
	OI::RpcData::HoldReq stBudgetMissingRequest;
	OI::RpcData::HoldRes stBudgetMissingOutput;
	stBudgetMissingOutput.iMark = -842;
	const int64_t iBudgetStartMilliseconds = OI::Timer::SteadyMilliseconds();
	auto stCachedCall = stGames.Call(1001, stBudgetRequest, stBudgetOutput, OI::Timer::Milliseconds(1000));
	auto stMissingCall = OI::Rpc::RpcCall(std::string("missing_budget.1"), stBudgetMissingRequest, stBudgetMissingOutput, OI::Timer::Milliseconds(2500));
	const int32_t iBudgetMissingCode = co_await stMissingCall;
	const long long iBudgetElapsedMilliseconds = OI::Timer::SteadyMilliseconds() - iBudgetStartMilliseconds;
	const int32_t iBudgetCachedCode = co_await stCachedCall;
	if (iBudgetMissingCode != OI_ERR_CONNECT || iBudgetElapsedMilliseconds < 500 || iBudgetElapsedMilliseconds >= 2450)
	{
		++iBudgetBad;
	}
	if (iBudgetCachedCode != OI_OK || stBudgetOutput.iMark != 841)
	{
		++iBudgetBad;
	}
	LOG_TRACE("hold registry budget miss code=%d elapsed_ms=%lld cached=%d mark=%d bad=%d",
	    iBudgetMissingCode, iBudgetElapsedMilliseconds, iBudgetCachedCode, stBudgetOutput.iMark, iBudgetBad);
	fflush(stdout);
	iBad += iBudgetBad;
	co_return iBad;
}

OI::Coro::TaskResult<int> RegistryMgr::RegistryLookups()
{
	int iBad = 0;
	for (int iRound = 0; iRound < 8; ++iRound)
	{
		// 读缓存会催一次后台拉取，和同步查询交错，回包按调用号配对。
		std::vector<OI::Base::ServiceInstance> vCached;
		OI::Base::RegistryClient::GetInstance().Snapshot("ipcserver_pairing_" + std::to_string(iRound), vCached);
		std::string sIp, sPort;
		if (OI::Base::RegistryClient::GetInstance().Lookup("ipcserver", sIp, sPort) != OI_OK || sIp != "127.0.0.1" || sPort != HoldTest::IpcServerPort())
		{
			++iBad;
		}
		co_await OI::Coro::yield(0);
	}
	co_return iBad;
}

OI::Coro::TaskResult<int> RegistryMgr::CheckRegistryPairing()
{
	std::vector<OI::Coro::TaskResult<int>> vLookups;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		vLookups.push_back(RegistryLookups());
	}
	int iBad = 0;
	for (const auto& stLookup : vLookups)
	{
		iBad += co_await stLookup;
	}
	LOG_TRACE("hold registry pairing bad=%d", iBad);
	co_return iBad;
}

// 8 路并发查 ipcserver，地址和端口都对上。
OI::Coro::TaskResult<int> RegistryMgr::RunPairing()
{
	co_return co_await CheckRegistryPairing();
}

// 暂停期间已缓存路由仍成功。未拉到过的名字超时，或在恢复后得到空名单。非法分区不改变结果条数。短暂停后未缓存名字等后台拉取。
OI::Coro::TaskResult<int> RegistryMgr::RunBounds()
{
	co_return co_await CheckRegistryBoundsAndBudget();
}
