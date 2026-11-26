#include "test_route.h"
#include "oi_log.h"
#include "oi_parser.h"
#include "oi_rpc_session_manager.h"
#include "oi_registry_client.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "rpc_ids.h"
#include "oi_duration.h"
#include <cstdio>
#include <string>
#include <vector>

OI::Coro::TaskResult<int> TestRoute::CheckRouteReconnect()
{
	int iBad = 0;
	OI::Rpc::RouteAwaiter stWait;
	stWait.sName = "gameserver";
	stWait.iPick = OI::Rpc::ServicePick::Zone;
	stWait.sZone = "1001";
	auto stRoute = co_await stWait;
	if (stRoute.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
	{
		LOG_TRACE("hold pooled reconnect bad=%d", 1);
		fflush(stdout);
		co_return 1;
	}
	auto* pClient = stRoute.vClients[0];
	OI::RpcData::HoldReq stRequest;
	OI::RpcData::HoldRes stOutput;
	stRequest.iMark = 845;
	if (co_await OI::Rpc::RpcCall(*pClient, stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 845)
	{
		++iBad;
	}
	for (int iRound = 0; iRound < 4; ++iRound)
	{
		pClient->Manager().CloseSession(OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pClient).iSessionId);
		// 不让出、不等 poll 清理：直接向同一连接发送，必须排队重连。
		stOutput.iMark = -845;
		if (co_await OI::Rpc::RpcCall(*pClient, stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 845)
		{
			++iBad;
		}
	}
	LOG_TRACE("hold pooled reconnect bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> TestRoute::CheckRouteBackpressure()
{
	int iBad = 0;
	OI::Rpc::RouteAwaiter stWait;
	stWait.sName = "gameserver";
	stWait.iPick = OI::Rpc::ServicePick::Zone;
	stWait.sZone = "1001";
	auto stRoute = co_await stWait;
	if (stRoute.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
	{
		LOG_TRACE("hold route backpressure status bad=%d", 1);
		fflush(stdout);
		co_return 1;
	}
	auto* pClient = stRoute.vClients[0];
	const auto stInfo = OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pClient);
	const size_t iSendBytes = stInfo.iSendBytes;
	const size_t iPendingBytes = stInfo.stPendingLimits.second;
	const size_t iCeiling = stInfo.iFrameBytes < iPendingBytes ? stInfo.iFrameBytes : iPendingBytes;
	OI::RpcData::HoldReq stWarmRequest;
	OI::RpcData::HoldRes stWarmResponse;
	stWarmRequest.iMark = 912;
	if (co_await OI::Rpc::RpcCall(*pClient, stWarmRequest, stWarmResponse, OI::Timer::Seconds(3)) != OI_OK || stWarmResponse.iMark != 912)
	{
		++iBad;
	}
	const auto iWarmSid = OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pClient).iSessionId;
	if (iWarmSid == INVALID_SID || iSendBytes == 0 || iSendBytes >= iCeiling)
	{
		++iBad;
	}
	else
	{
		auto fnFrameSize = [](const OI::RpcData::AllEchoReq& stRequest) -> size_t
		{
			OI::Base::BytesStream stBody;
			stBody << uint64_t(0);
			stRequest.serialize(stBody);
			OI::Base::BytesStream stFrame = OI::Rpc::EncodePbFrame(OI::Rpc::RpcProtoId<OI::RpcData::AllEchoReq>::iValue, stBody);
			return stFrame.GetMutableBuff().Size();
		};
		OI::RpcData::AllEchoReq stLargeRequest;
		size_t iLength = iSendBytes;
		stLargeRequest.stData.sMessage.assign(iLength, 'P');
		size_t iFrame = fnFrameSize(stLargeRequest);
		if (iFrame <= iSendBytes)
		{
			const size_t iGrow = iSendBytes - iFrame + 1;
			iLength += iGrow;
			stLargeRequest.stData.sMessage.assign(iLength, 'P');
			iFrame = fnFrameSize(stLargeRequest);
		}
		while (iFrame > iCeiling && iLength > 0)
		{
			const size_t iOver = iFrame - iCeiling;
			iLength = iLength > iOver ? iLength - iOver : 0;
			stLargeRequest.stData.sMessage.assign(iLength, 'P');
			iFrame = fnFrameSize(stLargeRequest);
		}
		OI::RpcData::AllEchoRes stOutput;
		stOutput.iCode = -7;
		if (iFrame <= iSendBytes || iFrame > iCeiling ||
		    co_await OI::Rpc::RpcCall(*pClient, stLargeRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_BUSY || stOutput.iCode != -7)
		{
			++iBad;
		}
		if (OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pClient).iSessionId != iWarmSid)
		{
			++iBad;
		}
		// 连接前缓存能装下该包，但连上之后的发送预算不足，也必须返回 BUSY。
		pClient->Manager().CloseSession(iWarmSid);
		stOutput.iCode = -7;
		if (co_await OI::Rpc::RpcCall(*pClient, stLargeRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_BUSY || stOutput.iCode != -7)
		{
			++iBad;
		}
		stWarmResponse.iMark = -912;
		if (co_await OI::Rpc::RpcCall(*pClient, stWarmRequest, stWarmResponse, OI::Timer::Seconds(3)) != OI_OK || stWarmResponse.iMark != 912)
		{
			++iBad;
		}
	}
	LOG_TRACE("hold route backpressure status bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> TestRoute::CheckRouteTargetStability()
{
	int iBad = 0;
	OI::Rpc::RouteAwaiter stPoolWait;
	stPoolWait.iKind = OI::Rpc::RouteAwaiter::Kind::Zones;
	stPoolWait.sName = "gameserver";
	stPoolWait.vZones = {1001, 1002};
	auto stPool = co_await stPoolWait;
	OI::Rpc::RouteAwaiter stSingleWait;
	stSingleWait.sName = "gameserver";
	stSingleWait.iPick = OI::Rpc::ServicePick::Zone;
	stSingleWait.sZone = "1001";
	auto stSingle = co_await stSingleWait;
	OI::Rpc::RouteAwaiter stManyWait;
	stManyWait.iKind = OI::Rpc::RouteAwaiter::Kind::Zones;
	stManyWait.sName = "gameserver";
	stManyWait.vZones = {1002, 1001};
	auto stMany = co_await stManyWait;
	auto* pZone1001 = stPool.vClients.size() > 0 ? stPool.vClients[0] : nullptr;
	auto* pZone1002 = stPool.vClients.size() > 1 ? stPool.vClients[1] : nullptr;
	if (stPool.iStatus != OI::Rpc::ServiceRouteStatus::Ok || !pZone1001 || !pZone1002 ||
	    stSingle.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stSingle.vClients.size() != 1 || stSingle.vClients[0] != pZone1001 ||
	    stMany.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stMany.vClients.size() != 2 || stMany.vClients[0] != pZone1002 ||
	    stMany.vClients[1] != pZone1001)
	{
		++iBad;
	}
	const auto iSid1001 = pZone1001 ? OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pZone1001).iSessionId : INVALID_SID;
	const auto iSid1002 = pZone1002 ? OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pZone1002).iSessionId : INVALID_SID;
	if (iSid1001 == INVALID_SID || iSid1002 == INVALID_SID)
	{
		++iBad;
	}
	co_await OI::Coro::yield(OI::Timer::Milliseconds(1100));
	OI::Rpc::RouteAwaiter stPoolAgain;
	stPoolAgain.iKind = OI::Rpc::RouteAwaiter::Kind::Zones;
	stPoolAgain.sName = "gameserver";
	stPoolAgain.vZones = {1001, 1002};
	auto stPoolAfter = co_await stPoolAgain;
	OI::Rpc::RouteAwaiter stSingleAgain;
	stSingleAgain.sName = "gameserver";
	stSingleAgain.iPick = OI::Rpc::ServicePick::Zone;
	stSingleAgain.sZone = "1001";
	auto stSingleAfter = co_await stSingleAgain;
	if (!pZone1001 || !pZone1002 || stPoolAfter.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stPoolAfter.vClients.size() != 2 ||
	    stPoolAfter.vClients[0] != pZone1001 || stPoolAfter.vClients[1] != pZone1002 || stSingleAfter.iStatus != OI::Rpc::ServiceRouteStatus::Ok ||
	    stSingleAfter.vClients.size() != 1 || stSingleAfter.vClients[0] != pZone1001 ||
	    OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pZone1001).iSessionId != iSid1001 ||
	    OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pZone1002).iSessionId != iSid1002)
	{
		++iBad;
	}
	if (pZone1001 && pZone1002 && iSid1002 != INVALID_SID)
	{
		std::vector<OI::Base::ServiceInstance> vSnapshot;
		const bool bSnapshot = OI::Base::RegistryClient::GetInstance().Snapshot("gameserver", vSnapshot);
		std::vector<OI::Base::ServiceInstance> vOldSnapshot;
		for (const auto& stInstance : vSnapshot)
		{
			if (stInstance.sZone == "1001")
			{
				vOldSnapshot.push_back(stInstance);
			}
		}
		if (!bSnapshot || vOldSnapshot.size() != 1)
		{
			++iBad;
		}
		else
		{
			OI::RpcData::HoldReq stDelayedRequest;
			OI::RpcData::HoldRes stDelayedResponse;
			stDelayedRequest.iDelayMilliseconds = 1000;
			stDelayedRequest.iMark = 1002;
			auto stDelayed = OI::Rpc::RpcCall(*pZone1002, stDelayedRequest, stDelayedResponse, OI::Timer::Seconds(3));
			co_await OI::Coro::yield(OI::Timer::Milliseconds(50));
			auto stOldRoute = OI::Rpc::RouteManager::GetInstance().Resolve("gameserver", OI::Rpc::ServicePick::Zone, "1001", OI_OK, vOldSnapshot);
			if (stOldRoute.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stOldRoute.vClients.size() != 1 ||
			    stOldRoute.vClients[0] != pZone1001 ||
			    OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pZone1002).iSessionId != iSid1002)
			{
				++iBad;
			}
			const int32_t iDelayedRet = co_await stDelayed;
			if (iDelayedRet != OI_OK || stDelayedResponse.iMark != 1002 ||
			    OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pZone1002).iSessionId != iSid1002)
			{
				++iBad;
			}
		}
	}
	else
	{
		++iBad;
	}
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 1001;
	if (stSingleAfter.vClients.size() == 1 && stSingleAfter.vClients[0])
	{
		auto stReply = co_await OI::Rpc::RpcCall(*stSingleAfter.vClients[0], stRequest, OI::Timer::Seconds(3));
		if (stReply.GetRet() != 0 || stReply.GetRes().iMark != stRequest.iMark)
		{
			++iBad;
		}
	}
	else
	{
		++iBad;
	}
	auto vReplies = co_await OI::Rpc::RouteCall::Send<OI::RpcData::HoldRes>(std::move(stPoolAfter), stRequest, OI::Timer::Seconds(3));
	if (vReplies.size() != 2)
	{
		++iBad;
	}
	for (const auto& stReply : vReplies)
	{
		if (stReply.GetRet() != 0 || stReply.GetRes().iMark != stRequest.iMark)
		{
			++iBad;
		}
	}
	LOG_TRACE("hold route target stability bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> TestRoute::RunTarget()
{
	co_return co_await CheckRouteTargetStability();
}

OI::Coro::TaskResult<int> TestRoute::RunPool()
{
	co_return co_await CheckRouteBackpressure();
}

OI::Coro::TaskResult<int> TestRoute::RunInbound()
{
	co_return co_await CheckRouteReconnect();
}
