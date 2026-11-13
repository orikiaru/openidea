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
	const auto stBefore = stRegistry.GetPullStats();
	if (!(co_await RegistryFaultHandshake(sFaultPath, "pause")))
	{
		co_return 1;
	}
	int iBusy = 0, iTimeout = 0;
	static ZoneService stManyProbe("gameserver");
	OI::RpcData::HoldReq stManyRequest;
	const std::vector<int32_t> vManyZones{1001, 1002, 1001};
	const auto vTimedOut = co_await stManyProbe.CallMany<OI::RpcData::HoldRes>(vManyZones, stManyRequest, OI::Timer::Milliseconds(100));
	int iManyBad = vTimedOut.size() == vManyZones.size() ? 0 : 1;
	for (const auto& stResult : vTimedOut)
	{
		iManyBad += stResult.GetRet() != OI_ERR_TIMEOUT;
	}
	struct PullProbe
	{
		std::atomic<int> iCompleted{0};
		std::atomic<int> iDuplicates{0};
		std::atomic<int> iBusy{0};
		std::atomic<int> iTimeout{0};
		std::atomic<int> iUnexpected{0};
	};
	std::vector<std::shared_ptr<PullProbe>> vProbes;
	for (int iRound = 0; iRound < 3; ++iRound)
	{
		auto pProbe = std::make_shared<PullProbe>();
		vProbes.push_back(pProbe);
		for (int iIndex = 0; iIndex < 64; ++iIndex)
		{
			auto pSeen = std::make_shared<std::atomic<bool>>(false);
			stRegistry.PullAsync("missing_timeout_" + std::to_string(iIndex), [pProbe, pSeen](int32_t iCode, const std::vector<OI::Base::ServiceInstance>&)
			{
				if (pSeen->exchange(true))
				{
					++pProbe->iDuplicates;
					return;
				}
				if (iCode == OI_ERR_BUSY)
				{
					++pProbe->iBusy;
				}
				else if (iCode == OI_ERR_TIMEOUT)
				{
					++pProbe->iTimeout;
				}
				else if (iCode != OI_ERR_CONNECT)
				{
					++pProbe->iUnexpected;
				}
				++pProbe->iCompleted;
			}, 200);
			const auto stStats = stRegistry.GetPullStats();
			if (stStats.iLiveCount > static_cast<size_t>(stStats.iMaxPendingPulls) ||
			    stStats.iLiveCount != stStats.iSentCount + stStats.iDeferredCount)
			{
				++iBad;
			}
		}
		if (iRound == 0)
		{
			const auto vBusy = co_await stManyProbe.CallMany<OI::RpcData::HoldRes>(vManyZones, stManyRequest, OI::Timer::Seconds(3));
			iManyBad += vBusy.size() != vManyZones.size();
			for (const auto& stResult : vBusy)
			{
				iManyBad += stResult.GetRet() != OI_ERR_BUSY;
			}
		}
		const int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
		while (pProbe->iCompleted.load() < 64 && OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
		{
			co_await OI::Coro::yield(10);
		}
		co_await OI::Coro::yield(100);
		if (pProbe->iCompleted.load() != 64 || pProbe->iDuplicates.load() != 0 || pProbe->iUnexpected.load() != 0 ||
		    stRegistry.GetPendingPullCount() != 0)
		{
			++iBad;
		}
		iBusy += pProbe->iBusy.load();
		iTimeout += pProbe->iTimeout.load();
	}
	if (iBusy == 0 || iTimeout == 0 || stRegistry.GetPullStats().iResetCount <= stBefore.iResetCount)
	{
		++iBad;
	}
	if (!(co_await RegistryFaultHandshake(sFaultPath, "resume")))
	{
		co_return iBad + 1;
	}
	// 让后台订阅拉取完成，再用真实已知分区验证恢复后回包配对。
	co_await OI::Coro::yield(1200);
	static ZoneService stGames("gameserver");
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 831;
	OI::RpcData::HoldRes stOutput;
	if (co_await stGames.Call(1001, stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 831)
	{
		++iBad;
	}
	for (const auto& pProbe : vProbes)
	{
		if (pProbe->iCompleted.load() != 64 || pProbe->iDuplicates.load() != 0 || pProbe->iUnexpected.load() != 0)
		{
			++iBad;
		}
	}
	const auto vInvalid = co_await stManyProbe.CallMany<OI::RpcData::HoldRes>({1001, -1, 1002}, stManyRequest, OI::Timer::Seconds(3));
	iManyBad += vInvalid.size() != 3;
	for (const auto& stResult : vInvalid)
	{
		iManyBad += stResult.GetRet() != OI_ERR_FAIL;
	}
	iBad += iManyBad;
	LOG_TRACE("hold batch failure slots bad=%d", iManyBad);
	LOG_TRACE("hold registry bounded recovery busy=%d timeout=%d bad=%d", iBusy, iTimeout, iBad);
	fflush(stdout);
	if (!(co_await RegistryFaultHandshake(sFaultPath, "budget")))
	{
		co_return iBad + 1;
	}
	// runner 在确认暂停后600ms恢复；查询+900ms服务处理超过1000ms总预算。
	stRequest.iDelayMilliseconds = 900;
	stOutput.iMark = -831;
	OI::RpcData::HoldRes stStringOutput;
	stStringOutput.iMark = -831;
	const int64_t iBudgetStartMilliseconds = OI::Timer::SteadyMilliseconds();
	auto stNamed = stGames.Call(1001, stRequest, stOutput, OI::Timer::Milliseconds(1000));
	auto stString = OI::Rpc::RpcCall(std::string("gameserver.1002"), stRequest, stStringOutput, OI::Timer::Milliseconds(1000));
	auto stValue = stGames.Call<OI::RpcData::HoldRes>(1001, stRequest, OI::Timer::Milliseconds(1000));
	auto stBatch = stGames.CallAll<OI::RpcData::HoldRes>(stRequest, OI::Timer::Milliseconds(1000));
	const auto iNamedCode = co_await stNamed;
	const auto iStringCode = co_await stString;
	const auto stValueResult = co_await stValue;
	const auto vBatchResults = co_await stBatch;
	const long long iBudgetElapsedMilliseconds = OI::Timer::SteadyMilliseconds() - iBudgetStartMilliseconds;
	if (iNamedCode != OI_ERR_TIMEOUT || iStringCode != OI_ERR_TIMEOUT || stValueResult.GetRet() != OI_ERR_TIMEOUT ||
	    stOutput.iMark != -831 || stStringOutput.iMark != -831 || vBatchResults.size() != 2 || iBudgetElapsedMilliseconds >= 1450)
	{
		++iBad;
	}
	for (const auto& stResult : vBatchResults)
	{
		if (stResult.GetRet() != OI_ERR_TIMEOUT)
		{
			++iBad;
		}
	}
	co_await OI::Coro::yield(1000);
	if (stOutput.iMark != -831 || stStringOutput.iMark != -831)
	{
		++iBad;
	}
	LOG_TRACE("hold named rpc end to end budget elapsed_ms=%lld bad=%d", iBudgetElapsedMilliseconds, iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> RegistryMgr::RegistryLookups()
{
	int iBad = 0;
	for (int iRound = 0; iRound < 8; ++iRound)
	{
		// 周期拉取与同步查询交错，队列顺序必须和线上顺序一致。
		OI::Base::RegistryClient::GetInstance().Pull();
		std::string sIp, sPort;
		if (!OI::Base::RegistryClient::GetInstance().Lookup("ipcserver", sIp, sPort) || sIp != "127.0.0.1" || sPort != HoldTest::IpcServerPort())
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

// 暂停 registry 制造忙和超时，恢复后回包仍配对。批量里夹非法分区时结果条数不变。查询加上处理可以超过端到端预算。
OI::Coro::TaskResult<int> RegistryMgr::RunBounds()
{
	co_return co_await CheckRegistryBoundsAndBudget();
}
