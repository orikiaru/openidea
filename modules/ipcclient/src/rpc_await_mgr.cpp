#include "rpc_await_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "rpc_probe.h"
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

template <typename T>
OI::Coro::TaskResult<bool> RpcAwaitMgr::AwaitSingleUse(std::shared_ptr<T> pCall)
{
	try
	{
		(void)(co_await *pCall);
	}
	catch (const std::logic_error&)
	{
		co_return false;
	}
	co_return true;
}

// 两次并发等待只能接受一次；完成后再等也必须拒绝。
template <typename T>
OI::Coro::TaskResult<int> RpcAwaitMgr::CheckSingleUse(std::shared_ptr<T> pCall)
{
	auto pReadyCount = std::make_shared<std::atomic<int>>(0);
	auto fnContend = [pCall, pReadyCount](OI::Coro::Token) -> OI::Coro::TaskResult<bool>
	{
		pReadyCount->fetch_add(1, std::memory_order_acq_rel);
		// 两个独立任务都就绪后再争抢；单线程等待时让出调度。
		while (pReadyCount->load(std::memory_order_acquire) < 2)
		{
			co_await OI::Coro::yield(0);
		}
		co_return co_await RpcAwaitMgr::GetInstance().AwaitSingleUse(pCall);
	};
	auto stFirst = OI::Coro::StartTask(fnContend);
	auto stSecond = OI::Coro::StartTask(fnContend);
	int iAccepted = (co_await stFirst ? 1 : 0) + (co_await stSecond ? 1 : 0);
	if (co_await AwaitSingleUse(pCall))
	{
		++iAccepted;
	}
	co_return iAccepted == 1 ? 0 : 1;
}

class SharedConnectProbe : public DeferredRpcProbe
{
  public:
	void Pending()
	{
		bConnectPending = true;
	}
	void Finish()
	{
		AbortSession(iConnectGeneration);
	}
	void OnConnectFailed() override
	{
	}
};

OI::Coro::TaskResult<int32_t> RpcAwaitMgr::AwaitConnect(std::shared_ptr<OI::Net::SessionManager::ConnectAwaiter> pConnect)
{
	co_return co_await *pConnect;
}

OI::Coro::TaskResult<int> RpcAwaitMgr::CheckAwaitOwnership()
{
	int iBad = 0;
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 50;
	stRequest.iMark = 123;
	// 先解码再移动响应：避免构造结果时提前取走仍未填充的对象。
	auto stDecodeProbe = OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest);
	OI::RpcData::HoldRes stDecodedSource;
	stDecodedSource.iMark = 456;
	stDecodeProbe.stReply << int32_t(0);
	stDecodedSource.serialize(stDecodeProbe.stReply);
	stDecodeProbe.pResponse = &stDecodeProbe.stResponse;
	stDecodeProbe.bReplied = true;
	auto stDecodedResult = stDecodeProbe.await_resume();
	if (stDecodedResult.GetRet() != 0 || stDecodedResult.GetRes().iMark != 456)
	{
		++iBad;
	}
	auto stRpc = OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, OI::Timer::Seconds(3));
	auto pRpc = std::make_shared<decltype(stRpc)>(std::move(stRpc));
	try
	{
		co_await stRpc;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	iBad += co_await CheckSingleUse(pRpc);
	OI::RpcData::HoldRes stOutput;
	auto stOutputWait = OI::Rpc::MakeRpcCallAwaiter(IpcClient::GetInstance(), stRequest, stOutput, OI::Timer::Seconds(3));
	iBad += co_await CheckSingleUse(std::make_shared<decltype(stOutputWait)>(std::move(stOutputWait)));
	if (stOutput.iMark != 123)
	{
		++iBad;
	}
	try
	{
		auto stInvalidMove = std::move(*pRpc);
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	OI::Http::Client stInvalidHttp("", 0);
	auto stHttp = stInvalidHttp.Get("/", {});
	auto pHttp = std::make_shared<OI::Http::Call>(std::move(stHttp));
	try
	{
		co_await stHttp;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	iBad += co_await CheckSingleUse(pHttp);
	int32_t iHttpPort = 0;
	TryGetConfig("Regression.http_port", iHttpPort);
	if (iHttpPort > 0)
	{
		OI::Http::Client stHttpClient("127.0.0.1", iHttpPort);
		iBad += co_await CheckSingleUse(std::make_shared<OI::Http::Call>(stHttpClient.Get("/good", {})));
	}
	static SharedConnectProbe stClient;
	OI::Net::Poll::GetInstance().CallOnLoop([](void*)
	{
		stClient.Pending();
	}, nullptr);
	auto pConnect = std::make_shared<OI::Net::SessionManager::ConnectAwaiter>(stClient.StartClient());
	// 每个等待者创建时就跑到挂起，循环结束后 32 个都已登记。
	std::vector<OI::Coro::TaskResult<int32_t>> vWaiters;
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		vWaiters.push_back(AwaitConnect(pConnect));
	}
	OI::Net::Poll::GetInstance().CallOnLoop([](void*)
	{
		stClient.Finish();
	}, nullptr);
	const auto vConnect = co_await OI::Coro::WhenAll(std::move(vWaiters));
	for (const auto iCode : vConnect)
	{
		if (iCode == OI_OK)
		{
			++iBad;
		}
	}
	if ((co_await *pConnect) == OI_OK)
	{
		++iBad;
	}
	auto stMovedConnect = std::move(*pConnect);
	try
	{
		co_await *pConnect;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	if ((co_await stMovedConnect) == OI_OK)
	{
		++iBad;
	}
	// 固定不存在的 sid 不得进入首次连接队列，也不能等待超时。
	static DeferredRpcProbe stPinnedClient;
	const auto stBefore = OI::Rpc::GetRpcWaitStats();
	auto stPinned = co_await OI::Rpc::RpcCall(stPinnedClient, sid_t(12345), stRequest, OI::Timer::Seconds(30));
	const auto stAfter = OI::Rpc::GetRpcWaitStats();
	if (stPinned.GetRet() != OI_ERR_CONNECT || stAfter.iCalls != stBefore.iCalls)
	{
		++iBad;
	}
	LOG_TRACE("hold await ownership shared=32 bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

// 单次等待器并发等只能成功一次。32 个协程共享同一次建连，移走后再等会拒绝。
OI::Coro::TaskResult<int> RpcAwaitMgr::Run()
{
	co_return co_await CheckAwaitOwnership();
}
