#include "hold_mgr.h"
#include "timer_mgr.h"
#include "nested_result_mgr.h"
#include "task_start_mgr.h"
#include "task_share_mgr.h"
#include "blocking_mgr.h"
#include "connect_mgr.h"
#include "address_mgr.h"
#include "isolation_mgr.h"
#include "backpressure_mgr.h"
#include "rpc_output_mgr.h"
#include "rpc_await_mgr.h"
#include "rpc_admission_mgr.h"
#include "route_mgr.h"
#include "registry_mgr.h"
#include "rpc_service_mgr.h"
#include "json_mgr.h"
#include "binary_mgr.h"
#include "encode_mgr.h"
#include "http_mgr.h"
#include "budget_mgr.h"
#include "schedule_mgr.h"
#include "rpc_pending_mgr.h"
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
#include <string>
#include <utility>
#include <vector>
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

HoldMgr::HoldMgr()
    : m_bEnabled(false), m_iSessionId(0), m_iRedisTimeoutMilliseconds(25000)
{
}

// 服务端反向 Hold 分支的接收端：回同一 mark，证明两个方向都能完成。
static std::atomic<int32_t> g_iReverseServed{0};

static ::OI::Rpc::RpcTask HandleReverseHold(const ::OI::RpcData::HoldReq& stRequest, ::OI::RpcData::HoldRes& stResponse)
{
	stResponse.iMark = stRequest.iMark;
	if (stRequest.iMark == -2147483598)
	{
		g_iReverseServed.fetch_add(1);
		LOG_TRACE("hold reverse request served delay=%d", stRequest.iDelayMilliseconds);
		fflush(stdout);
	}
	co_return OI_OK;
}

int32_t HoldMgr::Load()
{
	TryGetConfig("Test.hold", m_bEnabled);
	TryGetConfig("Regression.hold_scene", m_sScene);
	if (!m_sScene.empty() && m_sScene != "blocking" && m_sScene != "timer" && m_sScene != "redis")
	{
		LOG_ERROR("unknown Regression.hold_scene: %s", m_sScene.c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	TryGetConfig("RedisClient.timeout_ms", m_iRedisTimeoutMilliseconds);
	IpcClient::GetInstance().RegisterRpcHandler(HandleReverseHold);
	// 跳更新跟着 TimerMgr 的首次构造注册，必须赶在 RunFramework 之前。
	TimerMgr::GetInstance();
	return OI_OK;
}

void HoldMgr::Start(sid_t iSessionId)
{
	if (!m_bEnabled)
	{
		return;
	}
	m_iSessionId = iSessionId;
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return HoldMgr::GetInstance().Pump();
	});
}

OI::Coro::TaskResult<HoldMgr::RaceOutcome> HoldMgr::RaceOne(int32_t iMarkLocal, int32_t iDelay)
{
	::OI::RpcData::HoldReq stOne;
	stOne.iDelayMilliseconds = iDelay;
	stOne.iMark = iMarkLocal;
	auto stResult = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stOne, ::OI::Timer::Seconds(1));
	if (stResult.GetRet() == 0)
	{
		co_return stResult.GetRes().iMark == iMarkLocal ? RaceOutcome::Reply : RaceOutcome::Bad;
	}
	co_return stResult.GetRet() == OI_ERR_TIMEOUT ? RaceOutcome::Timeout : RaceOutcome::Bad;
}

OI::Coro::TaskResult<int> HoldMgr::CheckStandardizedReply()
{
	int iBadLocal = 0;
	auto& stManagerLocal = IpcClient::GetInstance();
	::OI::RpcData::HoldReq stRequestLocal;
	::OI::RpcData::HoldRes stResponseLocal;
	stRequestLocal.iDelayMilliseconds = 0;
	stRequestLocal.iMark = 1;
	auto stDirect = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequestLocal, ::OI::Timer::Seconds(10));
	if (!stDirect || stDirect.GetRes().iMark != 1)
	{
		++iBadLocal;
	}
	static_assert(std::is_same_v<decltype(stDirect.GetRet()), int32_t>);
	stRequestLocal.iDelayMilliseconds = -1;
	auto stFailed = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequestLocal);
	if (stFailed.GetRet() != 1001 || stFailed)
	{
		++iBadLocal;
	}
	bool bGuarded = false;
	try
	{
		(void)stFailed.GetRes();
	}
	catch (const std::logic_error&)
	{
		bGuarded = true;
	}
	if (!bGuarded)
	{
		++iBadLocal;
	}
	stResponseLocal.iMark = 876;
	int32_t iBusinessReturnCode = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, stResponseLocal);
	if (iBusinessReturnCode != 1001 || stResponseLocal.iMark != 876)
	{
		++iBadLocal;
	}
	stRequestLocal.iDelayMilliseconds = 0;
	iBusinessReturnCode = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, stResponseLocal, ::OI::Timer::Seconds(10));
	if (iBusinessReturnCode != 0 || stResponseLocal.iMark != 1)
	{
		++iBadLocal;
	}
	// 缺状态码和截断响应都应返回框架错误，不能覆盖应用已有响应。
	for (int iMalformed = 0; iMalformed < 2; ++iMalformed)
	{
		stResponseLocal.iMark = 876;
		auto stBroken = ::OI::Rpc::MakeRpcCallAwaiter(stManagerLocal, stRequestLocal, stResponseLocal);
		::OI::Base::BytesStream stReply;
		if (iMalformed)
		{
			stReply << int32_t(0) << uint8_t(1);
		}
		stBroken.DecodeReply(stReply, INVALID_SID);
		if (stBroken.await_resume() != OI_ERR_FAIL || stResponseLocal.iMark != 876)
		{
			++iBadLocal;
		}
	}
	auto fnValueTask = []() -> OI::Coro::TaskResult<int32_t>
	{
		co_await OI::Coro::yield(0);
		co_return 77;
	};
	if (co_await fnValueTask() != 77)
	{
		++iBadLocal;
	}
	auto fnThrowingTask = []() -> OI::Coro::TaskResult<int32_t>
	{
		co_await OI::Coro::yield(0);
		throw std::runtime_error("result task test");
		co_return 0;
	};
	bool bPropagated = false;
	try
	{
		(void)co_await fnThrowingTask();
	}
	catch (const std::runtime_error&)
	{
		bPropagated = true;
	}
	if (!bPropagated)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold rpc standardized reply bad=%d", iBadLocal);
	co_return iBadLocal;
}

// 超时后迟到的回包只能丢弃，不能配给后面的调用。
OI::Coro::TaskResult<int> HoldMgr::CheckRpcLateReply()
{
	int iBadLocal = 0;
	::OI::RpcData::HoldReq stRequestLocal;
	::OI::RpcData::HoldRes stResponseLocal;
	stRequestLocal.iMark = 1;
	stRequestLocal.iDelayMilliseconds = 600;
	auto stShortWait = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequestLocal, ::OI::Timer::Milliseconds(150));
	if (stShortWait.GetRet() != OI_ERR_TIMEOUT)
	{
		++iBadLocal;
	}
	co_await OI::Coro::yield(::OI::Timer::Milliseconds(700));
	stRequestLocal.iDelayMilliseconds = 0;
	auto stEarly = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequestLocal, ::OI::Timer::Seconds(10));
	int iReturnCodeLocal = stEarly.GetRet();
	if (iReturnCodeLocal == 0)
	{
		stResponseLocal = stEarly.GetRes();
	}
	LOG_TRACE("hold rpc early ret=%d mark=%d", iReturnCodeLocal, stResponseLocal.iMark);
	fflush(stdout);
	if (iReturnCodeLocal != 0 || stResponseLocal.iMark != 1)
	{
		iBadLocal += 1;
	}

	std::clock_t iCpuBegin = std::clock();
	int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	stRequestLocal.iDelayMilliseconds = 1500;
	stRequestLocal.iMark = 802;
	stResponseLocal.iMark = 0;
	auto stTimeout = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequestLocal, ::OI::Timer::Milliseconds(500));
	iReturnCodeLocal = stTimeout.GetRet();
	LOG_TRACE("hold rpc timeout ret=%d", iReturnCodeLocal);
	fflush(stdout);
	if (iReturnCodeLocal != OI_ERR_TIMEOUT)
	{
		iBadLocal += 1;
	}
	// 新调用保持在途，另发一个快速调用作为同连接上的收包屏障。
	stRequestLocal.iDelayMilliseconds = 3000;
	stRequestLocal.iMark = 803;
	::OI::RpcData::HoldRes stPendingResponse;
	auto stPending = ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequestLocal, stPendingResponse, ::OI::Timer::Seconds(10));
	co_await OI::Coro::yield(1500);
	::OI::RpcData::HoldReq stBarrierRequest;
	::OI::RpcData::HoldRes stBarrierResponse;
	stBarrierRequest.iMark = 804;
	stBarrierRequest.iDelayMilliseconds = 0;
	const int32_t iBarrierRet = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stBarrierRequest, stBarrierResponse, ::OI::Timer::Seconds(10));
	const size_t iPending = ::OI::Rpc::RpcCallManager::GetInstance().SessionCalls(&IpcClient::GetInstance(), m_iSessionId);
	const int32_t iFollowRet = co_await stPending;
	const bool bValid = iBarrierRet == OI_OK && stBarrierResponse.iMark == 804 && iPending == 1 &&
	    iFollowRet == OI_OK && stPendingResponse.iMark == 803;
	LOG_TRACE("hold rpc late overlap barrier=%d/%d pending=%zu follow=%d/%d bad=%d",
	    iBarrierRet, stBarrierResponse.iMark, iPending, iFollowRet, stPendingResponse.iMark, bValid ? 0 : 1);
	if (!bValid)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold rpc late-reply cpu_ms=%lld wall_ms=%lld",
	    HoldTest::CpuMs(iCpuBegin), HoldTest::WallMs(iWallBegin));
	co_return iBadLocal;
}

int HoldMgr::RaceBad(const std::vector<std::pair<int32_t, RaceOutcome>>& vOutcomes)
{
	std::vector<int32_t> vSeen;
	int iReply = 0;
	int iTimeout = 0;
	for (const auto& [iMark, iOutcome] : vOutcomes)
	{
		for (int32_t iSeen : vSeen)
		{
			if (iSeen == iMark)
			{
				return 1;
			}
		}
		vSeen.push_back(iMark);
		if (iOutcome == RaceOutcome::Reply)
		{
			++iReply;
		}
		else if (iOutcome == RaceOutcome::Timeout)
		{
			++iTimeout;
		}
		else
		{
			return 1;
		}
	}
	if (iReply < 1 || iTimeout < 1 || iReply + iTimeout != static_cast<int>(vOutcomes.size()))
	{
		return 1;
	}
	return 0;
}

// 服务端延迟落在客户端超时前后，每个调用只能拿到回包或超时之一。
OI::Coro::TaskResult<int> HoldMgr::CheckRpcRace()
{
	const int vDelays[] = {800, 900, 1000, 1100, 1200, 1400, 1600, 1800};
	const std::clock_t iCpuBegin = std::clock();
	const int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	std::vector<OI::Coro::TaskResult<RaceOutcome>> vCalls;
	for (int iIndexLocal = 0; iIndexLocal < 8; ++iIndexLocal)
	{
		vCalls.push_back(RaceOne(100 + iIndexLocal, vDelays[iIndexLocal]));
	}
	// 重复标记、错误内容、全超时都算失败。800ms 到 1800ms 只要求两种结局都出现，不按时间比例猜哪一次该回包。
	int iNegatives = 0;
	std::vector<std::pair<int32_t, RaceOutcome>> vAllTimeout;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		vAllTimeout.emplace_back(100 + iIndex, RaceOutcome::Timeout);
	}
	const std::vector<std::pair<int32_t, RaceOutcome>> vDuplicate = {
		{100, RaceOutcome::Reply}, {100, RaceOutcome::Reply}, {101, RaceOutcome::Timeout},
	};
	const std::vector<std::pair<int32_t, RaceOutcome>> vWrong = {
		{100, RaceOutcome::Bad}, {101, RaceOutcome::Reply}, {102, RaceOutcome::Timeout},
	};
	if (RaceBad(vAllTimeout) != 0)
	{
		++iNegatives;
	}
	if (RaceBad(vDuplicate) != 0)
	{
		++iNegatives;
	}
	if (RaceBad(vWrong) != 0)
	{
		++iNegatives;
	}
	std::vector<std::pair<int32_t, RaceOutcome>> vOutcomes;
	std::string sMarks;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		const int32_t iMark = 100 + iIndex;
		const RaceOutcome iOutcome = co_await vCalls[static_cast<size_t>(iIndex)];
		vOutcomes.emplace_back(iMark, iOutcome);
		if (!sMarks.empty())
		{
			sMarks.push_back(',');
		}
		sMarks += std::to_string(iMark);
		sMarks.push_back(':');
		sMarks.push_back(iOutcome == RaceOutcome::Reply ? 'R' : iOutcome == RaceOutcome::Timeout ? 'T' : 'B');
	}
	const RaceOutcome iControlledReply = co_await RaceOne(200, 0);
	const RaceOutcome iControlledTimeout = co_await RaceOne(201, 5000);
	int iReplyNumber = 0;
	int iTimeoutNumber = 0;
	for (const auto& [iMark, iOutcome] : vOutcomes)
	{
		(void)iMark;
		iReplyNumber += iOutcome == RaceOutcome::Reply ? 1 : 0;
		iTimeoutNumber += iOutcome == RaceOutcome::Timeout ? 1 : 0;
	}
	int iRaceBad = RaceBad(vOutcomes);
	if (iControlledReply != RaceOutcome::Reply || iControlledTimeout != RaceOutcome::Timeout || iNegatives != 3)
	{
		++iRaceBad;
	}
	LOG_TRACE("hold rpc race marks=%s reply=%d timeout=%d controlled_reply=%d controlled_timeout=%d negatives=%d bad=%d cpu_ms=%lld wall_ms=%lld",
	    sMarks.c_str(), iReplyNumber, iTimeoutNumber, iControlledReply == RaceOutcome::Reply ? 1 : 0,
	    iControlledTimeout == RaceOutcome::Timeout ? 1 : 0, iNegatives, iRaceBad, HoldTest::CpuMs(iCpuBegin), HoldTest::WallMs(iWallBegin));
	fflush(stdout);
	co_return iRaceBad;
}

OI::Coro::TaskResult<int> HoldMgr::CheckRedisPing(const char* pLabel)
{
	const int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	const auto stPing = co_await ::OI::Redis::CallRedis({"PING"});
	LOG_TRACE("hold redis %sping ok=%d status=%d reply_type=%d wall_ms=%lld", pLabel, HoldTest::Pong(stPing) ? 1 : 0,
	    static_cast<int>(stPing.iStatus), stPing.pReply ? stPing.pReply->type : -1, HoldTest::WallMs(iWallBegin));
	fflush(stdout);
	co_return HoldTest::Pong(stPing) ? 0 : 1;
}

// Redis 超时后迟到的回复不能串给下一条命令；回复与超时同时到也只能取其一。
OI::Coro::TaskResult<int> HoldMgr::CheckRedisLateReply(int32_t iRedisTimeoutMilliseconds)
{
	int iSpins = 0;
	int iLimit = iRedisTimeoutMilliseconds > 0 ? iRedisTimeoutMilliseconds / 50 : 100;
	if (iLimit < 1)
	{
		iLimit = 1;
	}
	while (!::OI::Redis::RedisClient::GetInstance().IsConnect())
	{
		if (iSpins++ >= iLimit)
		{
			LOG_TRACE("hold redis connect fail");
			fflush(stdout);
			co_return 1;
		}
		co_await OI::Coro::yield(50);
	}
	int iBadLocal = co_await CheckRedisPing("");
	int iSeconds = iRedisTimeoutMilliseconds / 1000;
	if (iSeconds < 1)
	{
		iSeconds = 1;
	}

	std::clock_t iCpuBegin = std::clock();
	int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	std::vector<std::string> vLate{"BLPOP", "hold-late", std::to_string(iSeconds + 1)};
	const auto stLate = co_await ::OI::Redis::CallRedis(std::move(vLate));
	LOG_TRACE("hold redis late timeout=%d cpu_ms=%lld wall_ms=%lld",
	    stLate.bTimeout ? 1 : 0, HoldTest::CpuMs(iCpuBegin), HoldTest::WallMs(iWallBegin));
	fflush(stdout);
	// 超时由框架以空回执结算，迟到的回复不能再挂到这次结果上。
	if (!stLate.bTimeout || stLate.pReply != nullptr)
	{
		iBadLocal += 1;
	}
	iBadLocal += co_await CheckRedisPing("late ");

	iCpuBegin = std::clock();
	iWallBegin = ::OI::Timer::SteadyMilliseconds();
	std::vector<std::string> vRace{"BLPOP", "hold-race", std::to_string(iSeconds)};
	const auto stRace = co_await ::OI::Redis::CallRedis(std::move(vRace));
	const int iNil = stRace.pReply && stRace.pReply->type == REDIS_REPLY_NIL ? 1 : 0;
	LOG_TRACE("hold redis race timeout=%d nil=%d cpu_ms=%lld wall_ms=%lld",
	    stRace.bTimeout ? 1 : 0, iNil, HoldTest::CpuMs(iCpuBegin), HoldTest::WallMs(iWallBegin));
	fflush(stdout);
	// 超时和 nil 回复恰好取其一：都没有是丢结果，超时还带着回执是两边都生效了。
	if ((!stRace.bTimeout && !iNil) || (stRace.bTimeout && stRace.pReply != nullptr))
	{
		iBadLocal += 1;
	}
	iBadLocal += co_await CheckRedisPing("race ");
	co_return iBadLocal;
}

OI::Coro::Task HoldMgr::CloseSessionLater(sid_t iSessionId)
{
	co_await OI::Coro::yield(::OI::Timer::Milliseconds(100));
	IpcClient::GetInstance().CloseSession(iSessionId);
}

// 真断开当前会话：已发出的 RPC 应及时失败，不能继续等十秒超时。
OI::Coro::TaskResult<int> HoldMgr::CheckRpcDisconnect(sid_t iSessionId)
{
	::OI::RpcData::HoldReq stRequestLocal;
	stRequestLocal.iDelayMilliseconds = 5000;
	stRequestLocal.iMark = 4;
	auto stClose = CloseSessionLater(iSessionId);
	const int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	auto stClosed = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequestLocal, ::OI::Timer::Seconds(10));
	const long long iWallMilliseconds = HoldTest::WallMs(iWallBegin);
	co_await stClose;
	LOG_TRACE("hold rpc disconnected code=%d wall_ms=%lld", stClosed.GetRet(), iWallMilliseconds);
	co_return (stClosed.GetRet() != OI_ERR_CONNECT || iWallMilliseconds > 2000) ? 1 : 0;
}

// 按固定顺序跑各主题 mgr，再测标准回包、迟到回包、回包撞车、Redis 迟到 PING 和断连收尾。
OI::Coro::Task HoldMgr::Pump()
{
	if (!m_sScene.empty())
	{
		int iBad = 0;
		if (m_sScene == "timer")
		{
			iBad = co_await TimerMgr::GetInstance().Run();
		}
		else if (m_sScene == "blocking")
		{
			iBad = co_await BlockingMgr::GetInstance().Run();
		}
		else
		{
			iBad = co_await CheckRedisLateReply(m_iRedisTimeoutMilliseconds);
		}
		LOG_TRACE("hold scene=%s bad=%d", m_sScene.c_str(), iBad);
		fflush(stdout);
		TestBoard::GetInstance().Done(iBad == 0 ? 0 : 1);
		co_return;
	}
	bool bAdmission = false;
	TryGetConfig("Regression.rpc_admission", bAdmission);
	if (bAdmission)
	{
		const int iBad = co_await RpcAdmissionMgr::GetInstance().Run();
		TestBoard::GetInstance().Done(iBad ? 1 : 0);
		co_return;
	}
	int iBad = co_await TimerMgr::GetInstance().Run();
	if (iBad != 0)
	{
		LOG_TRACE("hold done bad=%d", iBad);
		fflush(stdout);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	iBad += co_await NestedResultMgr::GetInstance().Run();
	iBad += co_await TaskStartMgr::GetInstance().RunWaiters();
	iBad += co_await TaskStartMgr::GetInstance().RunHot();
	iBad += co_await TaskShareMgr::GetInstance().Run();
	iBad += co_await BlockingMgr::GetInstance().Run();
	iBad += co_await TaskShareMgr::GetInstance().RunForeign();
	iBad += co_await RpcOutputMgr::GetInstance().RunDeferred();
	iBad += co_await RpcOutputMgr::GetInstance().Run(m_iSessionId);
	iBad += co_await RpcAwaitMgr::GetInstance().Run();
	iBad += BinaryMgr::GetInstance().Run();
	iBad += co_await ConnectMgr::GetInstance().Run();
	iBad += co_await AddressMgr::GetInstance().Run();
	iBad += co_await BackpressureMgr::GetInstance().Run();
	iBad += co_await IsolationMgr::GetInstance().Run();
	bool bRegistryChecks = false;
	TryGetConfig("Regression.registry_checks", bRegistryChecks);
	if (bRegistryChecks)
	{
		iBad += co_await RegistryMgr::GetInstance().RunPairing();
		iBad += co_await RpcServiceMgr::GetInstance().RunNamed();
		iBad += co_await RpcServiceMgr::GetInstance().RunOutput();
		iBad += co_await RpcServiceMgr::GetInstance().RunCompleted(m_iSessionId);
		iBad += co_await RouteMgr::GetInstance().RunPool();
		iBad += co_await RouteMgr::GetInstance().RunInbound();
		iBad += co_await RegistryMgr::GetInstance().RunBounds();
		iBad += co_await RouteMgr::GetInstance().RunTarget();
	}
	iBad += JsonMgr::GetInstance().Run();
	bool bEncodingRegression = false;
	TryGetConfig("Regression.encoding_failure", bEncodingRegression);
	if (bEncodingRegression)
	{
		iBad += co_await EncodeMgr::GetInstance().Run();
	}
	int32_t iHttpPort = 0;
	TryGetConfig("Regression.http_port", iHttpPort);
	if (iHttpPort > 0)
	{
		iBad += co_await HttpMgr::GetInstance().Run(iHttpPort);
	}
	iBad += BudgetMgr::GetInstance().Run();
	// 触发服务端反向 Hold：服务端收到这个 mark 会另起一条反向调用，客户端必须能应答。
	::OI::RpcData::HoldReq stReverse;
	stReverse.iMark = -2147483598;
	stReverse.iDelayMilliseconds = 0;
	::OI::RpcData::HoldRes stReverseReply;
	const int32_t iReverseRet = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), m_iSessionId, stReverse, stReverseReply, ::OI::Timer::Seconds(5));
	for (int32_t iWait = 0; iWait < 300 && g_iReverseServed.load() == 0; ++iWait)
	{
		co_await OI::Coro::yield(10);
	}
	const bool bReverse = iReverseRet == OI_OK && stReverseReply.iMark == stReverse.iMark && g_iReverseServed.load() > 0;
	LOG_TRACE("hold reverse trigger ret=%d mark=%d served=%d bad=%d", iReverseRet, stReverseReply.iMark,
	    g_iReverseServed.load(), bReverse ? 0 : 1);
	fflush(stdout);
	if (!bReverse)
	{
		++iBad;
	}
	iBad += co_await ScheduleMgr::GetInstance().RunFairness();
	iBad += co_await ScheduleMgr::GetInstance().RunScope();
	iBad += co_await ScheduleMgr::GetInstance().RunCaptured();
	iBad += co_await RpcPendingMgr::GetInstance().Run(m_iSessionId);
	iBad += co_await CheckStandardizedReply();
	iBad += co_await CheckRpcLateReply();
	iBad += co_await CheckRpcRace();
	iBad += co_await CheckRedisLateReply(m_iRedisTimeoutMilliseconds);
	iBad += co_await CheckRpcDisconnect(m_iSessionId);
	LOG_TRACE("hold done bad=%d", iBad);
	fflush(stdout);
	TestBoard::GetInstance().Done(iBad == 0 ? 0 : 1);
}
