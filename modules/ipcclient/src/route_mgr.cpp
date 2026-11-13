#include "route_mgr.h"
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

static std::atomic<int> iReverseStarted{0};
static std::atomic<int> iReverseFinished{0};

static OI_RPC_HANDLER(Hold, HandleReverseHold)
{
	++iReverseStarted;
	co_await OI::Coro::yield(stRequest.iDelayMilliseconds);
	stResponse.iMark = stRequest.iMark;
	++iReverseFinished;
	co_return 0;
}

OI::Coro::TaskResult<int32_t> RouteMgr::MoveRouteProbe(ZoneService* pManagerLocal, std::string sIpLocal, std::string sPortLocal)
{
	struct ServiceMove
	{
		ZoneService* pManager;
		std::string sIp;
		std::string sPort;
		std::exception_ptr stError;
		std::atomic<bool> bDone{false};
	};
	auto pMove = std::make_shared<ServiceMove>();
	pMove->pManager = pManagerLocal;
	pMove->sIp = std::move(sIpLocal);
	pMove->sPort = std::move(sPortLocal);
	// 帧保活状态直到 poll 发布完成；模拟注册更新，地址只在 poll 线程修改。
	OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		auto* pMoveLocal = static_cast<ServiceMove*>(pArgument);
		try
		{
			pMoveLocal->pManager->ApplyService(pMoveLocal->sIp, pMoveLocal->sPort, true);
		}
		catch (...)
		{
			pMoveLocal->stError = std::current_exception();
		}
		pMoveLocal->bDone.store(true, std::memory_order_release);
	}, pMove.get());
	while (!pMove->bDone.load(std::memory_order_acquire))
	{
		co_await OI::Coro::yield(1);
	}
	if (pMove->stError)
	{
		std::rethrow_exception(pMove->stError);
	}
	co_return co_await pManagerLocal->StartClient();
}

OI::Coro::TaskResult<int> RouteMgr::CheckRouteTargetStability()
{
	OI::Rpc::RouteAwaiter stPoolWait;
	stPoolWait.iKind = OI::Rpc::RouteAwaiter::Kind::Zones;
	stPoolWait.sName = "gameserver";
	stPoolWait.vZones = {1001, 1002};
	auto stPool = co_await stPoolWait;
	if (stPool.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stPool.vClients.size() != 2 ||
	    !stPool.vClients[0] || !stPool.vClients[1])
	{
		co_return 1;
	}
	static ZoneService stMoving("gameserver");
	auto* pFirst = stPool.vClients[0];
	auto* pSecond = stPool.vClients[1];
	if ((co_await MoveRouteProbe(&stMoving, OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pFirst).sIp, OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pFirst).sPort)) != OI_OK)
	{
		co_return 1;
	}
	OI::Rpc::RouteAwaiter stSingleWait;
	stSingleWait.pManager = &stMoving;
	stSingleWait.sName = "gameserver";
	stSingleWait.iPick = OI::Rpc::ServicePick::Zone;
	stSingleWait.sZone = "1001";
	auto stSingle = co_await stSingleWait;
	OI::Rpc::RouteAwaiter stManyWait;
	stManyWait.pManager = &stMoving;
	stManyWait.iKind = OI::Rpc::RouteAwaiter::Kind::Zones;
	stManyWait.sName = "gameserver";
	stManyWait.vZones = {1002, 1001};
	auto stMany = co_await stManyWait;
	// 刻意在选路和提交之间完成换区，稳定覆盖竞态窗口，不依赖压力碰运气。
	if ((co_await MoveRouteProbe(&stMoving, OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pSecond).sIp, OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pSecond).sPort)) != OI_OK)
	{
		co_return 1;
	}
	int iBad = 0;
	if (stSingle.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stSingle.vClients.size() != 1 ||
	    stSingle.vClients[0] != pFirst || stMany.iStatus != OI::Rpc::ServiceRouteStatus::Ok ||
	    stMany.vClients.size() != 2 || stMany.vClients[0] != pSecond || stMany.vClients[1] != pFirst)
	{
		++iBad;
	}
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 1001;
	if (stSingle.vClients.size() == 1 && stSingle.vClients[0])
	{
		auto stReply = co_await OI::Rpc::RpcCall(*stSingle.vClients[0], stRequest, OI::Timer::Seconds(3));
		if (stReply.GetRet() != 0 || stReply.GetRes().iMark != stRequest.iMark)
		{
			++iBad;
		}
	}
	auto vReplies = co_await OI::Rpc::SendRouted<OI::RpcData::HoldRes>(std::move(stMany), stRequest, OI::Timer::Seconds(3));
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
	stMoving.CloseAllSessions();
	LOG_TRACE("hold route target stability bad=%d", iBad);
	co_return iBad;
}

class RoutePolicyProbe : public OI::Rpc::ProtoClientManager
{
  public:
	explicit RoutePolicyProbe(size_t iScale)
	{
		sServiceName = "gameserver";
		iMaxSendQueueBytes = 256 * iScale;
		iMaxReceiveBufferBytes = 512 * iScale + 8;
		iMaxFrameBytes = 512 * iScale;
		iIncompleteFrameTimeoutMilliseconds = 300 * iScale;
		SetPendingCallLimits(2 * iScale, 1024 * iScale);
	}
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
	}
};

OI::Coro::TaskResult<int> RouteMgr::CheckRoutePoolLifetime()
{
	int iBad = 0;
	RoutePolicyProbe stSmall(1), stLarge(16);
	OI::Rpc::RouteAwaiter stSmallWait, stLargeWait;
	stSmallWait.pManager = &stSmall;
	stLargeWait.pManager = &stLarge;
	for (auto* pWait : {&stSmallWait, &stLargeWait})
	{
		pWait->sName = "gameserver";
		pWait->iPick = OI::Rpc::ServicePick::Zone;
		pWait->sZone = "1001";
	}
	auto stSmallRoute = co_await stSmallWait;
	auto stLargeRoute = co_await stLargeWait;
	if (stSmallRoute.vClients.size() != 1 || stLargeRoute.vClients.size() != 1 ||
	    stSmallRoute.vOwners.size() != 1 || stLargeRoute.vOwners.size() != 1)
	{
		co_return 1;
	}
	auto* pSmall = stSmallRoute.vClients[0];
	auto* pLarge = stLargeRoute.vClients[0];
	std::weak_ptr<void> pSmallOwner = OI::Rpc::RouteManager::GetInstance().ConnectionOwner(*pSmall);
	std::weak_ptr<void> pLargeOwner = OI::Rpc::RouteManager::GetInstance().ConnectionOwner(*pLarge);
	for (const auto& stPair : {std::make_pair(pSmall, &stSmall), std::make_pair(pLarge, &stLarge)})
	{
		if (OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*stPair.first).iSendBytes != stPair.second->MaxSendQueueBytes() ||
		    OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*stPair.first).iReceiveBytes != stPair.second->MaxReceiveBufferBytes() ||
		    OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*stPair.first).iFrameBytes != stPair.second->MaxFrameBytes() ||
		    OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*stPair.first).iIncompleteMilliseconds != stPair.second->IncompleteFrameTimeoutMilliseconds() ||
		    OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*stPair.first).stPendingLimits != stPair.second->PendingCallLimits())
		{
			++iBad;
		}
	}
	if (pSmall == pLarge || &pSmall->Manager() != &pLarge->Manager())
	{
		++iBad;
	}
	OI::RpcData::HoldReq stWarmRequest;
	OI::RpcData::HoldRes stWarmResponse;
	stWarmRequest.iMark = 912;
	if (co_await OI::Rpc::RpcCall(*pSmall, stWarmRequest, stWarmResponse, OI::Timer::Seconds(3)) != OI_OK)
	{
		++iBad;
	}
	const auto iWarmSid = OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pSmall).iSessionId;
	OI::RpcData::AllEchoReq stLargeRequest;
	stLargeRequest.stData.sMessage.assign(512, 'P');
	OI::RpcData::AllEchoRes stOutput;
	if (co_await OI::Rpc::RpcCall(*pSmall, stLargeRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_BUSY ||
	    co_await OI::Rpc::RpcCall(*pLarge, stLargeRequest, stOutput, OI::Timer::Seconds(3)) != 0 ||
	    stOutput.stData.sMessage != stLargeRequest.stData.sMessage)
	{
		++iBad;
	}
	if (OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pSmall).iSessionId != iWarmSid)
	{
		++iBad;
	}
	// 连接前缓存能装下该包，但连接后的发送预算不足，也必须准确返回 BUSY。
	pSmall->Manager().CloseSession(iWarmSid);
	if (co_await OI::Rpc::RpcCall(*pSmall, stLargeRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_BUSY ||
	    co_await OI::Rpc::RpcCall(*pSmall, stWarmRequest, stWarmResponse, OI::Timer::Seconds(3)) != OI_OK || stWarmResponse.iMark != 912)
	{
		++iBad;
	}
	LOG_TRACE("hold route backpressure status bad=%d", iBad);
	const auto stBefore = OI::Rpc::RouteManager::GetInstance().Stats();
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 800;
	stRequest.iMark = 821;
	OI::RpcData::HoldRes stHeldOutput;
	auto stHeld = OI::Rpc::RpcCall(*pLarge, stRequest, stHeldOutput, OI::Timer::Seconds(3));
	co_await OI::Coro::yield(OI::Timer::Milliseconds(600));
	if (OI::Rpc::RouteManager::GetInstance().Stats().iLeasedClients < 2 || co_await stHeld != 0 || stHeldOutput.iMark != 821)
	{
		++iBad;
	}
	// 弱引用指向业务lease，不把它变成额外保活引用；释放路由后等待真实闲置扫描。
	std::weak_ptr<OI::Rpc::RpcSender> pSmallLease = stSmallRoute.vOwners[0];
	std::weak_ptr<OI::Rpc::RpcSender> pLargeLease = stLargeRoute.vOwners[0];
	stSmallRoute = {};
	stLargeRoute = {};
	co_await OI::Coro::yield(OI::Timer::Milliseconds(600));
	const int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
	while ((!pSmallOwner.expired() || !pLargeOwner.expired()) && OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
	{
		co_await OI::Coro::yield(20);
	}
	const auto stAfter = OI::Rpc::RouteManager::GetInstance().Stats();
	if (!pSmallLease.expired() || !pLargeLease.expired() || !pSmallOwner.expired() || !pLargeOwner.expired() ||
	    stAfter.iEvicted < stBefore.iEvicted + 2 ||
	    stAfter.iClients > stAfter.iMaxClients)
	{
		++iBad;
	}
	stRequest.iDelayMilliseconds = 0;
	if (co_await stLarge.Call(1001, stRequest, stHeldOutput, OI::Timer::Seconds(3)) != 0 || stHeldOutput.iMark != 821)
	{
		++iBad;
	}
	LOG_TRACE("hold route policy isolation idle reconnect bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> RouteMgr::CheckRouteInboundAndReconnect()
{
	int iBad = 0;
	static ZoneService stGames("gameserver");
	OI::Rpc::RouteAwaiter stWait;
	stWait.pManager = &stGames;
	stWait.sName = "gameserver";
	stWait.iPick = OI::Rpc::ServicePick::Zone;
	stWait.sZone = "1001";
	auto stRoute = co_await stWait;
	if (stRoute.vClients.size() != 1)
	{
		co_return 1;
	}
	auto* pClient = stRoute.vClients[0];
	OI::RpcData::HoldReq stRequest;
	OI::RpcData::HoldRes stOutput;
	stRequest.iMark = 845;
	if (co_await OI::Rpc::RpcCall(*pClient, stRequest, stOutput, OI::Timer::Seconds(3)) != 0)
	{
		++iBad;
	}
	for (int iRound = 0; iRound < 4; ++iRound)
	{
		pClient->Manager().CloseSession(OI::Rpc::RouteManager::GetInstance().ConnectionInfo(*pClient).iSessionId);
		// 不让出、不等 poll 清理：直接向同一连接发送，必须排队重连。
		if (co_await OI::Rpc::RpcCall(*pClient, stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 845)
		{
			++iBad;
		}
	}
	LOG_TRACE("hold pooled reconnect bad=%d", iBad);
	std::weak_ptr<void> pWeakOwner = OI::Rpc::RouteManager::GetInstance().ConnectionOwner(*pClient);
	stRequest.iMark = -2147483598;
	if (co_await OI::Rpc::RpcCall(*pClient, stRequest, stOutput, OI::Timer::Seconds(3)) != 0)
	{
		++iBad;
	}
	const int64_t iStartDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
	while (iReverseStarted.load() == 0 && OI::Timer::SteadyMilliseconds() < iStartDeadlineMilliseconds)
	{
		co_await OI::Coro::yield(10);
	}
	stRoute = {}; // 唯一出站租约释放，反向处理仍应保活且阻止空闲关闭。
	co_await OI::Coro::yield(600);
	if (iReverseStarted.load() != 1 || iReverseFinished.load() != 0 || pWeakOwner.expired())
	{
		++iBad;
	}
	const int64_t iEndDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 5000;
	while (!pWeakOwner.expired() && OI::Timer::SteadyMilliseconds() < iEndDeadlineMilliseconds)
	{
		co_await OI::Coro::yield(20);
	}
	if (iReverseFinished.load() != 1 || !pWeakOwner.expired())
	{
		++iBad;
	}
	LOG_TRACE("hold inbound route lifetime bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> RouteMgr::CheckRoutePoolCapacity()
{
	int iBad = 0;
	int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
	while (OI::Rpc::RouteManager::GetInstance().Stats().iClients != 0 && OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
	{
		co_await OI::Coro::yield(20);
	}
	const auto stEmpty = OI::Rpc::RouteManager::GetInstance().Stats();
	if (stEmpty.iClients != 0 || stEmpty.iMaxClients != 8)
	{
		co_return 1;
	}
	OI::Base::ServiceInstance stInstance;
	stInstance.sName = "ipcserver";
	stInstance.iId = 1;
	stInstance.sIp = "127.0.0.1";
	stInstance.sPort = HoldTest::IpcServerPort();
	std::vector<OI::Base::ServiceInstance> vInstances{stInstance};
	std::vector<std::unique_ptr<RoutePolicyProbe>> vPolicies;
	std::vector<OI::Rpc::ServiceRoute> vRoutes;
	for (size_t iIndex = 0; iIndex < stEmpty.iMaxClients; ++iIndex)
	{
		vPolicies.push_back(std::make_unique<RoutePolicyProbe>(20 + iIndex));
		vRoutes.push_back(OI::Rpc::RouteManager::GetInstance().Resolve("ipcserver", OI::Rpc::ServicePick::Any, "", 0, vInstances, vPolicies.back().get()));
		if (vRoutes.back().iStatus != OI::Rpc::ServiceRouteStatus::Ok || vRoutes.back().vOwners.size() != 1)
		{
			++iBad;
		}
	}
	RoutePolicyProbe stOverflow(100);
	auto stRejected = OI::Rpc::RouteManager::GetInstance().Resolve("ipcserver", OI::Rpc::ServicePick::Any, "", 0, vInstances, &stOverflow);
	if (stRejected.iStatus != OI::Rpc::ServiceRouteStatus::Busy || !stRejected.vClients.empty() || !stRejected.vOwners.empty())
	{
		++iBad;
	}
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 822;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -822;
	const int32_t iBusy = co_await stOverflow.CallAny(stRequest, stOutput, OI::Timer::Seconds(3));
	if (iBusy != OI_ERR_BUSY || stOutput.iMark != -822 || OI::Rpc::RouteManager::GetInstance().Stats().iClients != 8)
	{
		++iBad;
	}
	// 池满不影响已持有的合法路由，且活跃租约跨扫描仍可真实收发。
	co_await OI::Coro::yield(600);
	if (vRoutes[0].vClients.size() == 1)
	{
		if (co_await OI::Rpc::RpcCall(*vRoutes[0].vClients[0], stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 822)
		{
			++iBad;
		}
	}
	vRoutes.clear();
	iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
	while (OI::Rpc::RouteManager::GetInstance().Stats().iClients != 0 && OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
	{
		co_await OI::Coro::yield(20);
	}
	if (OI::Rpc::RouteManager::GetInstance().Stats().iClients != 0 || co_await stOverflow.CallAny(stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 822)
	{
		++iBad;
	}
	LOG_TRACE("hold route pool capacity busy=%d bad=%d", iBusy == OI_ERR_BUSY ? 1 : 0, iBad);
	fflush(stdout);
	co_return iBad;
}

// 登记反向 Hold，供入站租约那一项使用。
void RouteMgr::Prepare()
{
	OI_RPC_BIND(Hold, HandleReverseHold);
}

// 选路之后、发送之前换区。单次和批量的目标连接保持选路当时的结果。
OI::Coro::TaskResult<int> RouteMgr::RunTarget()
{
	co_return co_await CheckRouteTargetStability();
}

// 不同发送策略互相隔离。租约还在时不回收，释放后连接销毁，下次调用重新建连。超预算回 BUSY。
OI::Coro::TaskResult<int> RouteMgr::RunPool()
{
	co_return co_await CheckRoutePoolLifetime();
}

// 同一条池连接断开后按需重连。出站租约释放时反向 RPC 仍挂着，回包成功后租约才结束。
OI::Coro::TaskResult<int> RouteMgr::RunInbound()
{
	co_return co_await CheckRouteInboundAndReconnect();
}

// 路由池满时新调用回 BUSY，腾出位置后小请求仍能完成。
OI::Coro::TaskResult<int> RouteMgr::RunCapacity()
{
	co_return co_await CheckRoutePoolCapacity();
}
