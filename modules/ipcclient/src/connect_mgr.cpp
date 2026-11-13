#include "connect_mgr.h"
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

class ConnectMgr::ClientConnectProbe : public OI::Net::SessionManager
{
	class ProbeSession : public OI::Net::NetSession
	{
	  public:
		explicit ProbeSession(ClientConnectProbe* pOwner)
		    : NetSession(pOwner)
		{
		}
		void OnDataIn() override
		{
			stInputBuffer.Clear();
		}
		void OnDataOut() override
		{
		}
	};
	std::atomic<int> m_iConnections{0};
	bool m_bReconnectInCallback;
	std::optional<ConnectAwaiter> m_stRetry;
	OI::Net::Session* CloneSession() override
	{
		return new ProbeSession(this);
	}
	void OnAddSession(OI::Net::Session* pSession) override
	{
		if (++m_iConnections == 1 && m_bReconnectInCallback)
		{
			CloseSession(pSession->GetSessionID());
			DefaultInit("127.0.0.1", "0");
			m_stRetry.emplace(StartClient());
		}
	}
	void OnDelSession(const OI::Net::Session*) override
	{
	}

  public:
	explicit ClientConnectProbe(const std::string& sPortLocal, bool bReconnect = false)
	    : m_bReconnectInCallback(bReconnect)
	{
		DefaultInit("127.0.0.1", sPortLocal);
	}
	ConnectAwaiter& Retry()
	{
		return m_stRetry.value();
	}
	int Connections() const
	{
		return m_iConnections.load();
	}
};

OI::Coro::TaskResult<int32_t> ConnectMgr::AwaitClientConnect(ClientConnectProbe* pProbe)
{
	co_return co_await pProbe->StartClient();
}

OI::Coro::TaskResult<int> ConnectMgr::CheckClientConnect()
{
	static ClientConnectProbe stGood(HoldTest::IpcServerPort()), stRefused("0");
	int iBad = 0;
	static ClientConnectProbe stEager(HoldTest::IpcServerPort()), stDelayedAwait(HoldTest::IpcServerPort());
	stEager.StartClient();
	auto stConnecting = stDelayedAwait.StartClient();
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	while ((stEager.Connections() != 1 || stDelayedAwait.Connections() != 1) && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await OI::Coro::yield(1);
	}
	if (stEager.Connections() != 1 || stDelayedAwait.Connections() != 1 || (co_await stConnecting) != OI_OK)
	{
		++iBad;
	}
	if ((co_await stConnecting) != OI_OK)
	{
		++iBad;
	}
	stEager.CloseAllSessions();
	stDelayedAwait.CloseAllSessions();
	if ((co_await stGood.StartClient()) != OI_OK || stGood.Connections() != 1)
	{
		++iBad;
	}
	if ((co_await stGood.StartClient()) != OI_OK || stGood.Connections() != 1)
	{
		++iBad;
	}
	if ((co_await stRefused.StartClient()) == OI_OK)
	{
		++iBad;
	}
	struct Result
	{
		std::atomic<bool> bDone{false};
		int iBad = 0;
		bool bReturned = false;
	};
	auto pResult = std::make_shared<Result>();
	OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgument)
	{
		auto* pState = static_cast<Result*>(pArgument);
		OI::Net::Poll::GetInstance().PostOnLoop([](void* pNestedLocal)
		{
			auto* pNested = static_cast<Result*>(pNestedLocal);
			if (!pNested->bReturned)
			{
				++pNested->iBad;
			}
			pNested->bDone = true;
		}, pState);
		pState->bReturned = true;
	}, pResult.get());
	while (!pResult->bDone.load())
	{
		co_await OI::Coro::yield(1);
	}
	static ClientConnectProbe stAwait(HoldTest::IpcServerPort()), stAwaitRefused("0");
	std::vector<OI::Coro::TaskResult<int32_t>> vConnects;
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		vConnects.push_back(AwaitClientConnect(&stAwait));
	}
	for (const auto& stConnect : vConnects)
	{
		const int32_t iCode = co_await stConnect;
		if (iCode != OI_OK)
		{
			++iBad;
		}
	}
	if (stAwait.Connections() != 1 || (co_await stAwait.StartClient()) != OI_OK)
	{
		++iBad;
	}
	if ((co_await stAwaitRefused.StartClient()) == OI_OK)
	{
		++iBad;
	}
	static ClientConnectProbe stReentrant(HoldTest::IpcServerPort(), true);
	const bool bFirst = (co_await stReentrant.StartClient()) == OI_OK;
	const bool bRetry = (co_await stReentrant.Retry()) == OI_OK;
	const int iReentrantBad = (!bFirst || bRetry || stReentrant.Connections() != 1) ? 1 : 0;
	iBad += iReentrantBad;
	LOG_TRACE("hold reentrant connect first=%d retry=%d connections=%d bad=%d", bFirst, bRetry, stReentrant.Connections(), iReentrantBad);
	stAwait.CloseAllSessions();
	stGood.CloseAllSessions();
	iBad += pResult->iBad;
	LOG_TRACE("hold coroutine connect bad=%d", iBad);
	co_return iBad;
}

// 协程建连、拒绝，以及 32 个等待者共用一条连接。回调里再次 StartClient 时，新一代收不到旧连接的成功。
OI::Coro::TaskResult<int> ConnectMgr::Run()
{
	co_return co_await CheckClientConnect();
}
