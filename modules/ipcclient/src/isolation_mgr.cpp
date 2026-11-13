#include "isolation_mgr.h"
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


// 在 poll 执行域里模拟旧建连完成/失败，不能取消当前代次或新增会话。
class ConnectGenerationProbe : public OI::Net::SessionManager
{
	int m_iFailures = 0;
	OI::Net::Session* CloneSession() override
	{
		return nullptr;
	}
	void OnAddSession(OI::Net::Session*) override
	{
	}
	void OnDelSession(const OI::Net::Session*) override
	{
	}
	void OnConnectFailed() override
	{
		++m_iFailures;
	}

  public:
	int Check()
	{
		iConnectGeneration = 2;
		bConnectPending = true;
		AbortSession(1);
		const auto iDescriptor = NetOs_CreateTcp4(true);
		if (iDescriptor == INVALID_SOCKET_ID)
		{
			return 1;
		}
		CompleteConnect(iDescriptor, 1);
		int iBad = (!bConnectPending || m_iFailures != 0 || !mSessionMap.empty()) ? 1 : 0;
		AbortSession(2);
		AbortSession(2);
		if (bConnectPending || m_iFailures != 1)
		{
			++iBad;
		}
		return iBad;
	}
};

// 重复 StartClient 不能顶掉已有连接；RPC 和 Redis 各自收到自己的回包。
OI::Coro::TaskResult<int> IsolationMgr::IsolationCall(int iIndex)
{
	int iBad = 0;
	IpcClient::GetInstance().StartClient();
	OI::Redis::RedisClient::GetInstance().StartClient();
	OI::Log::LogClient::GetInstance().StartClient();
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 10000 + iIndex;
	auto stResult = co_await IpcClient::GetInstance().CallAny<OI::RpcData::HoldRes>(stRequest, OI::Timer::Seconds(3));
	if (stResult.GetRet() != 0 || stResult.GetRes().iMark != stRequest.iMark)
	{
		++iBad;
	}
	std::vector<std::string> vArguments{"ECHO", std::to_string(iIndex)};
	auto stReply = co_await OI::Redis::CallRedis(std::move(vArguments));
	if (!stReply || !stReply.pReply->str || std::string(stReply.pReply->str, stReply.pReply->len) != std::to_string(iIndex))
	{
		++iBad;
	}
	co_return iBad;
}

OI::Coro::TaskResult<int> IsolationMgr::CheckConnectionIsolation()
{
	int iBad = 0;
	auto& stClient = IpcClient::GetInstance();
	auto& stRedis = OI::Redis::RedisClient::GetInstance();
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(5)).iMilliseconds;
	while (!stRedis.IsConnect() && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await OI::Coro::yield(1);
	}
	const sid_t iRpcSid = stClient.ConnectedSid();
	const sid_t iRedisSid = stRedis.ConnectedSid();
	if (iRpcSid == INVALID_SID || iRedisSid == INVALID_SID)
	{
		co_return 1;
	}
	std::vector<OI::Coro::TaskResult<int>> vCalls;
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		vCalls.push_back(IsolationCall(iIndex));
	}
	for (const auto& stCall : vCalls)
	{
		iBad += co_await stCall;
	}
	if (stClient.ConnectedSid() != iRpcSid || stRedis.ConnectedSid() != iRedisSid)
	{
		++iBad;
	}
	struct ProbeResult
	{
		std::atomic<bool> bDone{false};
		int iBad = 0;
	};
	auto pProbe = std::make_shared<ProbeResult>();
	// 协程帧持有 pProbe，回调完成前不退出；回调结束后只读已发布的结果。
	OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		auto* pResult = static_cast<ProbeResult*>(pArgument);
		ConnectGenerationProbe stProbe;
		pResult->iBad = stProbe.Check();
		auto& stRedisLocal = OI::Redis::RedisClient::GetInstance();
		stRedisLocal.Feed(INVALID_SID, "not RESP", 8);
		stRedisLocal.Feed(stRedisLocal.ConnectedSid() + 1, "+FOREIGN\r\n", 10);
		pResult->bDone.store(true);
	}, pProbe.get());
	while (!pProbe->bDone.load())
	{
		co_await OI::Coro::yield(1);
	}
	if (!HoldTest::Pong(co_await OI::Redis::CallRedis({"PING"})))
	{
		++iBad;
	}
	iBad += pProbe->iBad;
	LOG_TRACE("hold connection isolation bad=%d", iBad);
	co_return iBad;
}

// 重复 StartClient 不顶掉已有连接。RPC 和 Redis 各自只收到自己的回包。
OI::Coro::TaskResult<int> IsolationMgr::Run()
{
	co_return co_await CheckConnectionIsolation();
}
