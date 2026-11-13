#include "address_mgr.h"
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


// 字面地址当场用，域名交给 StartClient 解析。监听口用配置端口加 17，避开回归脚本的端口表。
class AddressMgr::AddressConnectProbe : public OI::Net::SessionManager
{
	class ProbeSession : public OI::Net::NetSession
	{
	  public:
		explicit ProbeSession(AddressConnectProbe* pOwner)
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
	OI::Net::Session* CloneSession() override
	{
		return new ProbeSession(this);
	}
	void OnAddSession(OI::Net::Session*) override
	{
		++m_iConnections;
	}
	void OnDelSession(const OI::Net::Session*) override
	{
	}
	// StartServer 会按管理器名字发布。返回 Registry 时发布直接成功，避免把 ipcclient 登记到测试端口。
	const std::string GetIDString() override
	{
		return "Registry";
	}

  public:
	explicit AddressConnectProbe(const std::string& sIpLocal, const std::string& sPortLocal)
	{
		DefaultInit(sIpLocal, sPortLocal);
	}
	int Connections() const
	{
		return m_iConnections.load();
	}
};

std::string AddressMgr::Ipv6ListenPort()
{
	const std::string sIpcPort = HoldTest::IpcServerPort();
	char* pEnd = nullptr;
	const long iPort = std::strtol(sIpcPort.c_str(), &pEnd, 10);
	if (pEnd == sIpcPort.c_str() || pEnd == nullptr || *pEnd != '\0' || iPort <= 0 || iPort > 65535 - 17)
	{
		return std::string();
	}
	return std::to_string(iPort + 17);
}

int AddressMgr::CheckLiteralAddress(const std::string& sPort)
{
	int iBad = 0;
	OI::Net::NetAddr stLoopback;
	if (!stLoopback.Init("127.0.0.1", sPort) || stLoopback.Endpoint() != "127.0.0.1:" + sPort || stLoopback.IsUnspecified())
	{
		++iBad;
	}
	OI::Net::NetAddr stV6;
	if (!stV6.Init("::1", sPort) || stV6.Endpoint() != "[::1]:" + sPort || stV6.IsUnspecified())
	{
		++iBad;
	}
	OI::Net::NetAddr stBracket;
	if (!stBracket.Init("[::1]", sPort) || stBracket.Endpoint() != "[::1]:" + sPort || stBracket.IsUnspecified())
	{
		++iBad;
	}
	OI::Net::NetAddr stAny4;
	if (!stAny4.Init("0.0.0.0", sPort) || !stAny4.IsUnspecified())
	{
		++iBad;
	}
	OI::Net::NetAddr stAny6;
	if (!stAny6.Init("::", sPort) || !stAny6.IsUnspecified())
	{
		++iBad;
	}
	OI::Net::NetAddr stAnyBracket;
	if (!stAnyBracket.Init("[::]", sPort) || !stAnyBracket.IsUnspecified())
	{
		++iBad;
	}
	if (OI::Net::NetAddr::Endpoint("localhost", sPort) != "localhost:" + sPort)
	{
		++iBad;
	}
	return iBad;
}

OI::Coro::TaskResult<int> AddressMgr::AwaitAddressConnect(AddressConnectProbe* pProbe)
{
	co_return (co_await pProbe->StartClient()) ? 0 : 1;
}

OI::Coro::TaskResult<int> AddressMgr::CheckAddressConnect()
{
	const std::string sPort = HoldTest::IpcServerPort();
	const std::string sListenPort = Ipv6ListenPort();
	int iBad = CheckLiteralAddress(sPort);
	static AddressConnectProbe stLocal("localhost", sPort);
	if (!(co_await stLocal.StartClient()) || stLocal.Connections() != 1)
	{
		++iBad;
	}
	if (!(co_await stLocal.StartClient()) || stLocal.Connections() != 1)
	{
		++iBad;
	}
	stLocal.CloseAllSessions();
	static AddressConnectProbe stServer("::1", sListenPort);
	if (!stServer.StartServer(true))
	{
		++iBad;
	}
	static AddressConnectProbe stV6("::1", sListenPort);
	static AddressConnectProbe stBracket("[::1]", sListenPort);
	if (!(co_await stV6.StartClient()) || stV6.Connections() != 1)
	{
		++iBad;
	}
	if (!(co_await stBracket.StartClient()) || stBracket.Connections() != 1)
	{
		++iBad;
	}
	stV6.CloseAllSessions();
	stBracket.CloseAllSessions();
	static AddressConnectProbe stMissing("no-such-host.invalid", sPort);
	if (co_await stMissing.StartClient())
	{
		++iBad;
	}
	stMissing.CloseAllSessions();
	LOG_TRACE("hold address connect bad=%d", iBad);
	static AddressConnectProbe stStress("localhost", sPort);
	std::vector<OI::Coro::TaskResult<int>> vConnects;
	for (int iIndex = 0; iIndex < 64; ++iIndex)
	{
		vConnects.push_back(AwaitAddressConnect(&stStress));
	}
	int iStressBad = 0;
	for (const auto& stConnect : vConnects)
	{
		iStressBad += co_await stConnect;
	}
	if (stStress.Connections() != 1)
	{
		++iStressBad;
	}
	for (int iRound = 0; iRound < 20; ++iRound)
	{
		stStress.CloseAllSessions();
		if (!(co_await stStress.StartClient()))
		{
			++iStressBad;
		}
	}
	stStress.CloseAllSessions();
	LOG_TRACE("hold address stress waiters=64 rounds=20 bad=%d", iStressBad);
	co_return iBad + iStressBad;
}

// 字面地址、域名、IPv6 和方括号写法，失败主机，以及 64 个等待者反复开关连接。
OI::Coro::TaskResult<int> AddressMgr::Run()
{
	co_return co_await CheckAddressConnect();
}
