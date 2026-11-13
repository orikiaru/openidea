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

void HoldMgr::Load()
{
	TryGetConfig("Test.hold", m_bEnabled);
	if (m_bEnabled)
	{
		RouteMgr::Prepare();
	}
	TryGetConfig("RedisClient.timeout_ms", m_iRedisTimeoutMilliseconds);
	// 跳更新跟着 TimerMgr 的首次构造注册，必须赶在 RunFramework 之前。
	TimerMgr::GetInstance();
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
	auto stResult = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stOne, ::OI::Timer::Seconds(1));
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
	auto stDirect = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
	if (!stDirect || stDirect.GetRes().iMark != 1)
	{
		++iBadLocal;
	}
	static_assert(std::is_same_v<decltype(stDirect.GetRet()), int32_t>);
	stRequestLocal.iDelayMilliseconds = -1;
	auto stFailed = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal);
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
		stBroken.bReplied = true;
		if (iMalformed)
		{
			stBroken.stReply << int32_t(0) << uint8_t(1);
		}
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
	auto stShortWait = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Milliseconds(150));
	if (stShortWait.GetRet() != OI_ERR_TIMEOUT)
	{
		++iBadLocal;
	}
	co_await OI::Coro::yield(::OI::Timer::Milliseconds(700));
	stRequestLocal.iDelayMilliseconds = 0;
	auto stEarly = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
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
	stRequestLocal.iMark = 2;
	stResponseLocal.iMark = 0;
	auto stTimeout = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Milliseconds(500));
	iReturnCodeLocal = stTimeout.GetRet();
	LOG_TRACE("hold rpc timeout ret=%d", iReturnCodeLocal);
	fflush(stdout);
	if (iReturnCodeLocal != OI_ERR_TIMEOUT)
	{
		iBadLocal += 1;
	}
	// 回包还在路上。让这一下把迟到的包和已经武装的超时定时器都消化掉。
	co_await OI::Coro::yield(1500);
	LOG_TRACE("hold rpc late-reply cpu_ms=%lld wall_ms=%lld",
	    HoldTest::CpuMs(iCpuBegin), HoldTest::WallMs(iWallBegin));
	fflush(stdout);

	stRequestLocal.iDelayMilliseconds = 0;
	stRequestLocal.iMark = 3;
	stResponseLocal.iMark = 0;
	auto stFollow = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
	iReturnCodeLocal = stFollow.GetRet();
	stResponseLocal.iMark = iReturnCodeLocal == 0 ? stFollow.GetRes().iMark : 0;
	LOG_TRACE("hold rpc follow ret=%d mark=%d", iReturnCodeLocal, stResponseLocal.iMark);
	fflush(stdout);
	if (iReturnCodeLocal != 0 || stResponseLocal.iMark != 3)
	{
		iBadLocal += 1;
	}
	co_return iBadLocal;
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
	int iReplyNumber = 0, iTimeoutNumber = 0, iRaceBad = 0;
	for (const auto& stCall : vCalls)
	{
		const RaceOutcome iOutcome = co_await stCall;
		iReplyNumber += iOutcome == RaceOutcome::Reply ? 1 : 0;
		iTimeoutNumber += iOutcome == RaceOutcome::Timeout ? 1 : 0;
		iRaceBad += iOutcome == RaceOutcome::Bad ? 1 : 0;
	}
	LOG_TRACE("hold rpc race n=%zu done=%zu reply=%d timeout=%d bad=%d cpu_ms=%lld wall_ms=%lld",
	    vCalls.size(), vCalls.size(), iReplyNumber, iTimeoutNumber, iRaceBad, HoldTest::CpuMs(iCpuBegin), HoldTest::WallMs(iWallBegin));
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
	if (!stLate.bTimeout)
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
	if (!stRace.bTimeout && !iNil)
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
	auto stClosed = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
	const long long iWallMilliseconds = HoldTest::WallMs(iWallBegin);
	co_await stClose;
	LOG_TRACE("hold rpc disconnected code=%d wall_ms=%lld", stClosed.GetRet(), iWallMilliseconds);
	co_return (stClosed.GetRet() != OI_ERR_CONNECT || iWallMilliseconds > 2000) ? 1 : 0;
}

// 按固定顺序跑各主题 mgr，再测标准回包、迟到回包、回包撞车、Redis 迟到 PING 和断连收尾。
OI::Coro::Task HoldMgr::Pump()
{
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
		iBad += co_await RouteMgr::GetInstance().RunCapacity();
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
