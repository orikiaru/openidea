#include "oi_rpc_coro.h"
#include "oi_config.h"
#include "oi_thread.h"
#include "oi_tcp_channel.h"
#include <tuple>
#include <mutex>
#include <cstdlib>
#include <uv.h>
namespace OI
{
namespace Rpc
{
namespace
{
bool ValidServiceToken(const std::string& sTextParam)
{
	if (sTextParam.empty())
	{
		return false;
	}
	for (size_t iIndexLocal = 0; iIndexLocal < sTextParam.size(); ++iIndexLocal)
	{
		char iCharacter = sTextParam[iIndexLocal];
		bool bOk = (iCharacter >= 'a' && iCharacter <= 'z') || (iCharacter >= 'A' && iCharacter <= 'Z') || (iCharacter >= '0' && iCharacter <= '9') || iCharacter == '_' || iCharacter == '-';
		if (!bOk)
		{
			return false;
		}
	}
	return true;
}

struct ParsedTarget
{
	std::string sName;
	std::string sZone;
	bool bAll = false;
};

bool ParseTarget(const std::string& sTargetParam, ParsedTarget& stTargetParam)
{
	if (sTargetParam.size() >= 3 && sTargetParam.compare(sTargetParam.size() - 2, 2, ".*") == 0)
	{
		stTargetParam.sName = sTargetParam.substr(0, sTargetParam.size() - 2);
		stTargetParam.bAll = true;
		return ValidServiceToken(stTargetParam.sName);
	}
	size_t iDot = sTargetParam.find('.');
	if (iDot == std::string::npos)
	{
		stTargetParam.sName = sTargetParam;
		return ValidServiceToken(stTargetParam.sName);
	}
	if (sTargetParam.find('.', iDot + 1) != std::string::npos)
	{
		return false;
	}
	stTargetParam.sName = sTargetParam.substr(0, iDot);
	stTargetParam.sZone = sTargetParam.substr(iDot + 1);
	return ValidServiceToken(stTargetParam.sName) && ValidServiceToken(stTargetParam.sZone);
}

struct RoutePolicy
{
	size_t iSendBytes = 64 * 1024 * 1024;
	size_t iReceiveBytes = 64 * 1024 * 1024 + 8;
	size_t iFrameBytes = 64 * 1024 * 1024;
	uint64_t iIncompleteMilliseconds = 10000;
	size_t iHeldCount = 65536;
	size_t iHeldBytes = 64 * 1024 * 1024;

	explicit RoutePolicy(::OI::Net::SessionManager* pManagerParam)
	{
		if (pManagerParam)
		{
			iSendBytes = pManagerParam->MaxSendQueueBytes();
			iReceiveBytes = pManagerParam->MaxReceiveBufferBytes();
			iFrameBytes = pManagerParam->MaxFrameBytes();
			iIncompleteMilliseconds = pManagerParam->IncompleteFrameTimeoutMilliseconds();
			if (auto* pClient = dynamic_cast<ProtoClientManager*>(pManagerParam))
			{
				std::tie(iHeldCount, iHeldBytes) = pClient->PendingCallLimits();
			}
		}
	}
	bool operator<(const RoutePolicy& stOtherParam) const
	{
		return std::tie(iSendBytes, iReceiveBytes, iFrameBytes, iIncompleteMilliseconds, iHeldCount, iHeldBytes) <
		       std::tie(stOtherParam.iSendBytes, stOtherParam.iReceiveBytes, stOtherParam.iFrameBytes, stOtherParam.iIncompleteMilliseconds, stOtherParam.iHeldCount, stOtherParam.iHeldBytes);
	}
};

struct RouteConnection;

// 进程内只有这一个路由管理器；所有连接共用 SID 空间和断连清理。
class RouteManager final : public ProtoSessionManager
{
	RouteManager() = default;
	RouteManager(const RouteManager&) = delete;
	RouteManager& operator=(const RouteManager&) = delete;

  public:
	static RouteManager& Instance()
	{
		static RouteManager stManager;
		return stManager;
	}
	sid_t Adopt(net_socket_t iSocketParam, const std::shared_ptr<RouteConnection>& pConnectionParam);
	const std::string GetIDString() override
	{
		return "RpcRoute";
	}
	void OnAddSession(::OI::Net::Session*) override
	{
	}
	void OnDelSession(const ::OI::Net::Session*) override
	{
	}
};

class RouteConnector;
struct RouteConnection
{
	struct Held
	{
		uint64_t iCallId;
		::OI::Base::Bytes stData;
	};
	std::weak_ptr<RouteConnection> pSelf;
	RpcSender stSender;
	const std::string sIp;
	const std::string sPort;
	const RoutePolicy stPolicy;
	::OI::Thread::Mutex stMutex;
	std::weak_ptr<RpcSender> pLease;
	sid_t iSessionId = INVALID_SID;
	bool bConnecting = false;
	bool bPosted = false;
	bool bRetired = false;
	RouteConnector* pConnector = nullptr; // 只在 poll 线程操作。
	std::list<Held> vHeld;
	std::unordered_map<uint64_t, std::list<Held>::iterator> mHeld;
	size_t iHeldBytes = 0;

	RouteConnection(std::string sIpParam, std::string sPortParam, RoutePolicy stPolicyParam)
	    : sIp(std::move(sIpParam)), sPort(std::move(sPortParam)), stPolicy(stPolicyParam)
	{
		stSender = {&RouteManager::Instance(), this, [](void* pContextParam, uint64_t iCallIdParam, const ::OI::Base::Bytes& stDataParam)
		{
			static_cast<RouteConnection*>(pContextParam)->Submit(iCallIdParam, stDataParam);
		}, [](void* pContextParam, uint64_t iCallIdParam)
		{
			static_cast<RouteConnection*>(pContextParam)->Drop(iCallIdParam);
		}};
	}
	std::shared_ptr<RpcSender> AcquireLease()
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		auto pCurrent = pLease.lock();
		if (!pCurrent)
		{
			// 业务引用独立于 socket/会话持有的连接状态，避免空闲连接永远不回收。
			struct Lease
			{
				std::shared_ptr<RouteConnection> pConnection;
			};
			auto pOwner = std::make_shared<Lease>(Lease{pSelf.lock()});
			pCurrent = std::shared_ptr<RpcSender>(pOwner, &stSender);
			pLease = pCurrent;
		}
		return pCurrent;
	}
	bool HasLease()
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		return !pLease.expired();
	}
	void Ensure();
	void Begin();
	void Retire();
	void Connected(net_socket_t iSocketParam);
	void Finish(sid_t iSessionIdParam)
	{
		std::list<Held> vReady;
		{
			::OI::Thread::MutexGuard stLock(stMutex);
			bConnecting = false;
			iSessionId = iSessionIdParam;
			vReady.swap(vHeld);
			mHeld.clear();
			iHeldBytes = 0;
		}
		for (const auto& stCall : vReady)
		{
			if (iSessionIdParam == INVALID_SID)
			{
				WakeRpcCall(stCall.iCallId, OI_ERR_CONNECT, nullptr);
			}
			else if (ClaimRpcSend(stCall.iCallId, iSessionIdParam))
			{
				const int32_t iResult = RouteManager::Instance().SendBytesResult(iSessionIdParam, stCall.stData);
				if (iResult != OI_OK)
				{
					WakeRpcCall(stCall.iCallId, iResult, nullptr);
				}
			}
		}
	}
	void Disconnected(sid_t iSessionIdParam)
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		if (iSessionId == iSessionIdParam)
		{
			iSessionId = INVALID_SID;
		}
	}
	void Drop(uint64_t iCallIdParam)
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		auto it = mHeld.find(iCallIdParam);
		if (it != mHeld.end())
		{
			iHeldBytes -= it->second->stData.Size();
			vHeld.erase(it->second);
			mHeld.erase(it);
		}
	}
	void Submit(uint64_t iCallIdParam, const ::OI::Base::Bytes& stDataParam)
	{
		sid_t iTarget = INVALID_SID;
		int32_t iError = OI_OK;
		{
			::OI::Thread::MutexGuard stLock(stMutex);
			if (bRetired)
			{
				iError = OI_ERR_CONNECT;
			}
			else if (iSessionId != INVALID_SID)
			{
				iTarget = iSessionId;
				if (!ClaimRpcSend(iCallIdParam, iTarget))
				{
					return;
				}
			}
			else if (vHeld.size() >= stPolicy.iHeldCount || iHeldBytes > stPolicy.iHeldBytes || stDataParam.Size() > stPolicy.iHeldBytes - iHeldBytes)
			{
				iError = OI_ERR_BUSY;
			}
			else if (MarkRpcAwaitingConnect(iCallIdParam))
			{
				vHeld.push_back({iCallIdParam, stDataParam});
				try
				{
					mHeld.emplace(iCallIdParam, std::prev(vHeld.end()));
				}
				catch (...)
				{
					vHeld.pop_back();
					throw;
				}
				iHeldBytes += stDataParam.Size();
			}
			else
			{
				return;
			}
		}
		if (iError != OI_OK)
		{
			WakeRpcCall(iCallIdParam, iError, nullptr);
		}
		else if (iTarget != INVALID_SID)
		{
			const int32_t iResult = RouteManager::Instance().SendBytesResult(iTarget, stDataParam);
			if (iResult != OI_OK)
			{
				WakeRpcCall(iCallIdParam, iResult, nullptr);
			}
		}
		else
		{
			Ensure();
		}
	}
};

class RouteSession final : public ProtoSession
{
	std::shared_ptr<RouteConnection> m_pConnection;

  public:
	RouteSession(RouteManager* pManagerParam, std::shared_ptr<RouteConnection> pConnectionParam)
	    : ProtoSession(pManagerParam), m_pConnection(std::move(pConnectionParam))
	{
		SetFrameLimits(m_pConnection->stPolicy.iFrameBytes, m_pConnection->stPolicy.iIncompleteMilliseconds);
	}
	std::shared_ptr<void> HoldTask() override
	{
		return m_pConnection->AcquireLease();
	}
	void ActiveClose() override
	{
		// 逻辑断连即撤销发送目标；不能等 poll 回收通道后才允许下次调用重连。
		m_pConnection->Disconnected(GetSessionID());
		ProtoSession::ActiveClose();
	}
	void OnClose() override
	{
		m_pConnection->Disconnected(GetSessionID());
		ProtoSession::OnClose();
	}
};

sid_t RouteManager::Adopt(net_socket_t iSocketParam, const std::shared_ptr<RouteConnection>& pConnectionParam)
{
	struct SocketOwner
	{
		net_socket_t iSocket;
		~SocketOwner()
		{
			if (iSocket != INVALID_SOCKET_ID)
			{
				NetOs_Close(iSocket);
			}
		}
	} stOwner{iSocketParam};
	auto pSession = std::make_unique<RouteSession>(this, pConnectionParam);
	::OI::Net::NetSocket stSocket;
	stSocket.Attach(iSocketParam);
	auto pChannel = std::make_unique<::OI::Net::TCP_Channel>(stSocket, pSession.get());
	stOwner.iSocket = INVALID_SOCKET_ID; // 从这里起由通道关闭描述符。
	pSession->SetChannel(pChannel.get());
	AddSession(pSession.get(), pConnectionParam->sIp + ":" + pConnectionParam->sPort,
	    pConnectionParam->stPolicy.iSendBytes, pConnectionParam->stPolicy.iReceiveBytes);
	const sid_t iSessionId = pSession->GetSessionID();
	try
	{
		::OI::Net::Poll::GetInstance().IOEventRegister(pChannel.get(), true, false);
	}
	catch (...)
	{
		CloseSession(iSessionId);
		throw;
	}
	auto* pPublished = pChannel.release();
	pSession.release();
	if (!pPublished->pPollToken)
	{
		CloseSession(iSessionId);
		pPublished->TryClose();
		return INVALID_SID;
	}
	return iSessionId;
}

// 仅负责一次 TCP 建连，不是管理器。成功后沿用框架 TCP_Channel/ProtoSession 收发。
class RouteConnector final : public ::OI::Net::Channel
{
	std::shared_ptr<RouteConnection> m_pConnection;
	bool m_bClosed = false;

  public:
	RouteConnector(::OI::Net::NetSocket stSocketParam, std::shared_ptr<RouteConnection> pConnectionParam)
	    : Channel(stSocketParam, nullptr), m_pConnection(std::move(pConnectionParam))
	{
	}
	void DataIn() override
	{
		DataOut();
	}
	void DataOut() override
	{
		if (m_bClosed)
		{
			return;
		}
		const int iError = NetOs_GetSocketError(stSocket.GetSocket());
		if (iError == EINPROGRESS || iError == EINTR)
		{
			return;
		}
		m_bClosed = true;
		m_pConnection->pConnector = nullptr;
		// 与普通建连一样交 dup 的描述符，旧 poll 注销不会移除新会话的监听。
		const net_socket_t iSocket = iError ? INVALID_SOCKET_ID : NetOs_Dup(stSocket.GetSocket());
		m_pConnection->Connected(iSocket);
	}
	bool TryClose() override
	{
		if (!m_bClosed)
		{
			return false;
		}
		::OI::Net::Poll::GetInstance().IOEventUnRegister(this);
		delete this;
		return true;
	}
	void Cancel()
	{
		m_bClosed = true;
		TryClose();
	}
};

void RouteConnection::Connected(net_socket_t iSocketParam)
{
	if (iSocketParam == INVALID_SOCKET_ID)
	{
		Finish(INVALID_SID);
		return;
	}
	sid_t iConnected = INVALID_SID;
	try
	{
		iConnected = RouteManager::Instance().Adopt(iSocketParam, pSelf.lock());
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
	}
	Finish(iConnected);
}

void RouteConnection::Begin()
{
	assert(::OI::Net::Poll::GetInstance().InLoopThread());
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		bPosted = false;
		if (bRetired || bConnecting || iSessionId != INVALID_SID)
		{
			return;
		}
		bConnecting = true;
	}
	sockaddr_in stAddress{};
	stAddress.sin_family = AF_INET;
	char* pEnd = nullptr;
	const long iPort = strtol(sPort.c_str(), &pEnd, 10);
	if (sPort.empty() || *pEnd || iPort < 1 || iPort > 65535 || inet_pton(AF_INET, sIp.c_str(), &stAddress.sin_addr) != 1)
	{
		Finish(INVALID_SID);
		return;
	}
	stAddress.sin_port = htons(static_cast<uint16_t>(iPort));
	::OI::Net::NetSocket stSocket;
	stSocket.Attach(NetOs_CreateTcp(true));
	if (stSocket.GetSocket() == INVALID_SOCKET_ID)
	{
		Finish(INVALID_SID);
		return;
	}
	const int iResult = NetOs_Connect(stSocket.GetSocket(), reinterpret_cast<sockaddr*>(&stAddress), sizeof(stAddress));
	if (iResult == 0)
	{
		Connected(stSocket.GetSocket());
	}
	else if (errno == EINPROGRESS)
	{
		try
		{
			pConnector = new RouteConnector(stSocket, pSelf.lock());
		}
		catch (...)
		{
			NetOs_Close(stSocket.GetSocket());
			throw;
		}
		try
		{
			::OI::Net::Poll::GetInstance().IOEventRegister(pConnector, false, true);
		}
		catch (...)
		{
			auto* pCancel = std::exchange(pConnector, nullptr);
			pCancel->Cancel();
			throw;
		}
		if (pConnector && !pConnector->pPollToken)
		{
			auto* pCancel = std::exchange(pConnector, nullptr);
			pCancel->Cancel();
			Finish(INVALID_SID);
		}
	}
	else
	{
		NetOs_Close(stSocket.GetSocket());
		Finish(INVALID_SID);
	}
}

void RouteConnection::Ensure()
{
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		if (bRetired || bConnecting || bPosted || iSessionId != INVALID_SID)
		{
			return;
		}
		bPosted = true;
	}
	try
	{
		auto pRequest = std::make_unique<std::shared_ptr<RouteConnection>>(pSelf.lock());
		::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgumentParam)
		{
			std::unique_ptr<std::shared_ptr<RouteConnection>> pHeld(static_cast<std::shared_ptr<RouteConnection>*>(pArgumentParam));
			try
			{
				(*pHeld)->Begin();
			}
			catch (...)
			{
				(*pHeld)->Finish(INVALID_SID);
				::OI::Thread::ReportTaskError(std::current_exception());
			}
		}, pRequest.get());
		pRequest.release();
	}
	catch (...)
	{
		{
			::OI::Thread::MutexGuard stLock(stMutex);
			bPosted = false;
		}
		throw;
	}
}

void RouteConnection::Retire()
{
	assert(::OI::Net::Poll::GetInstance().InLoopThread());
	sid_t iSession;
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		bRetired = true;
		iSession = iSessionId;
	}
	if (pConnector)
	{
		auto* pCancel = std::exchange(pConnector, nullptr);
		pCancel->Cancel();
	}
	if (iSession != INVALID_SID)
	{
		RouteManager::Instance().CloseSession(iSession);
	}
	Finish(INVALID_SID);
}

struct RoutePool
{
	struct Entry
	{
		std::shared_ptr<RouteConnection> pClient;
		int64_t iIdleSinceMilliseconds = 0;
	};
	::OI::Thread::Mutex stMutex;
	std::map<std::tuple<std::string, std::string, RoutePolicy>, Entry> mClients;
	size_t iMaxClients = 1024;
	size_t iEvicted = 0;
	int64_t iIdleMilliseconds = 60000;
	uv_timer_t stTimer{};
	bool bTimerInitialized = false;
	bool bStopped = false;
	std::once_flag stArmOnce;

	RoutePool()
	{
		int64_t iLimit = static_cast<int64_t>(iMaxClients);
		TryGetConfig("RpcRoute.max_clients", iLimit);
		if (iLimit <= 0 || static_cast<uint64_t>(iLimit) > SIZE_MAX)
		{
			throw std::invalid_argument("RpcRoute.max_clients");
		}
		iMaxClients = static_cast<size_t>(iLimit);
		TryGetConfig("RpcRoute.idle_ms", iIdleMilliseconds);
		if (iIdleMilliseconds <= 0)
		{
			throw std::invalid_argument("RpcRoute.idle_ms");
		}
	}
};

RoutePool& RoutedPool()
{
	static RoutePool stPool;
	return stPool;
}

void SweepRoutePool(bool bStopParam)
{
	assert(::OI::Net::Poll::GetInstance().InLoopThread());
	auto& stPool = RoutedPool();
	std::vector<std::shared_ptr<RouteConnection>> vRetired;
	const int64_t iNow = ::OI::Timer::SteadyMilliseconds();
	{
		::OI::Thread::MutexGuard stLock(stPool.stMutex);
		vRetired.reserve(stPool.mClients.size());
		if (bStopParam)
		{
			stPool.bStopped = true;
		}
		for (auto it = stPool.mClients.begin(); it != stPool.mClients.end();)
		{
			auto& stEntry = it->second;
			if (!bStopParam && stEntry.pClient->HasLease())
			{
				stEntry.iIdleSinceMilliseconds = 0;
				++it;
				continue;
			}
			if (stEntry.iIdleSinceMilliseconds == 0)
			{
				stEntry.iIdleSinceMilliseconds = iNow;
			}
			if (!bStopParam && iNow - stEntry.iIdleSinceMilliseconds < stPool.iIdleMilliseconds)
			{
				++it;
				continue;
			}
			vRetired.push_back(stEntry.pClient);
			it = stPool.mClients.erase(it);
			++stPool.iEvicted;
		}
	}
	// 不在池锁内结束等待：恢复的协程可能马上再次选路。
	for (const auto& pClient : vRetired)
	{
		pClient->Retire();
	}
}

void StopRoutePool()
{
	auto& stPool = RoutedPool();
	if (stPool.bTimerInitialized)
	{
		uv_timer_stop(&stPool.stTimer);
	}
	SweepRoutePool(true);
}

void ArmRoutePool(void*)
{
	auto& stPool = RoutedPool();
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested())
	{
		StopRoutePool();
		return;
	}
	if (uv_timer_init(::OI::Net::Poll::GetInstance().Loop(), &stPool.stTimer) != 0)
	{
		StopRoutePool();
		return;
	}
	stPool.bTimerInitialized = true;
	const uint64_t iIntervalMilliseconds = static_cast<uint64_t>((std::min)(stPool.iIdleMilliseconds, int64_t{1000}));
	if (uv_timer_start(&stPool.stTimer, [](uv_timer_t*)
	{
		try
		{
			SweepRoutePool(false);
		}
		catch (...)
		{
			::OI::Thread::ReportTaskError(std::current_exception());
		}
	}, iIntervalMilliseconds, iIntervalMilliseconds) != 0)
	{
		StopRoutePool();
	}
}

std::shared_ptr<RpcSender> ClientFor(const std::string& sIpParam, const std::string& sPortParam, ::OI::Net::SessionManager* pManagerParam)
{
	auto& stPool = RoutedPool();
	std::call_once(stPool.stArmOnce, []()
	{
		if (::OI::Thread::ThreadPool::GetInstance().AddStopHandler(StopRoutePool))
		{
			::OI::Net::Poll::GetInstance().CallOnLoop(ArmRoutePool, nullptr);
		}
	});
	const auto stKey = std::make_tuple(sIpParam, sPortParam, RoutePolicy(pManagerParam));
	std::shared_ptr<RpcSender> pLease;
	{
		::OI::Thread::MutexGuard stLock(stPool.stMutex);
		if (stPool.bStopped || ::OI::Thread::ThreadPool::GetInstance().StopRequested())
		{
			return {};
		}
		auto it = stPool.mClients.find(stKey);
		if (it == stPool.mClients.end())
		{
			if (stPool.mClients.size() >= stPool.iMaxClients)
			{
				return {};
			}
			auto pClient = std::make_shared<RouteConnection>(sIpParam, sPortParam, std::get<2>(stKey));
			pClient->pSelf = pClient;
			it = stPool.mClients.emplace(stKey, RoutePool::Entry{std::move(pClient), 0}).first;
		}
		auto& stEntry = it->second;
		// 入站处理和出站路由共用业务租约，网络保活不计入空闲判断。
		pLease = stEntry.pClient->AcquireLease();
		stEntry.iIdleSinceMilliseconds = 0;
	}
	// 连接自行合并建连请求；管理器始终是进程常驻单例。
	static_cast<RouteConnection*>(pLease->pContext)->Ensure();
	return pLease;
}

bool AddRouteClient(ServiceRoute& stRouteParam, const std::string& sIpParam, const std::string& sPortParam, ::OI::Net::SessionManager* pManagerParam)
{
	auto pOwner = ClientFor(sIpParam, sPortParam, pManagerParam);
	if (!pOwner)
	{
		stRouteParam.vClients.clear();
		stRouteParam.vOwners.clear();
		stRouteParam.iStatus = ServiceRouteStatus::Busy;
		return false;
	}
	stRouteParam.vClients.push_back(pOwner.get());
	stRouteParam.vOwners.push_back(std::move(pOwner));
	return true;
}
}

RouteConnectionInfo GetRouteConnectionInfo(const RpcSender& stSenderParam)
{
	if (stSenderParam.pManager != &RouteManager::Instance())
	{
		return {};
	}
	auto* pConnection = static_cast<RouteConnection*>(stSenderParam.pContext);
	::OI::Thread::MutexGuard stLock(pConnection->stMutex);
	const auto& stPolicy = pConnection->stPolicy;
	return {pConnection->sIp, pConnection->sPort, pConnection->iSessionId, stPolicy.iSendBytes,
	    stPolicy.iReceiveBytes, stPolicy.iFrameBytes, stPolicy.iIncompleteMilliseconds,
	    {stPolicy.iHeldCount, stPolicy.iHeldBytes}};
}

std::shared_ptr<void> GetRouteConnectionOwner(const RpcSender& stSenderParam)
{
	if (stSenderParam.pManager != &RouteManager::Instance())
	{
		return {};
	}
	return static_cast<RouteConnection*>(stSenderParam.pContext)->pSelf.lock();
}

RoutePoolStats GetRoutePoolStats()
{
	auto& stPool = RoutedPool();
	::OI::Thread::MutexGuard stLock(stPool.stMutex);
	RoutePoolStats stStats;
	stStats.iClients = stPool.mClients.size();
	stStats.iMaxClients = stPool.iMaxClients;
	stStats.iIdleMilliseconds = stPool.iIdleMilliseconds;
	stStats.iEvicted = stPool.iEvicted;
	for (const auto& [stKey, stEntry] : stPool.mClients)
	{
		if (stEntry.pClient->HasLease())
		{
			++stStats.iLeasedClients;
		}
	}
	return stStats;
}

bool PrepareRoute(::OI::Net::SessionManager* pManagerParam, const std::string& sNameParam, ServicePick iPickParam, const std::string& sZoneParam, ServiceRoute& stRouteParam)
{
	if (!ValidServiceToken(sNameParam) || (iPickParam == ServicePick::Zone && !ValidServiceToken(sZoneParam)))
	{
		stRouteParam.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	if (iPickParam == ServicePick::Any && pManagerParam)
	{
		std::vector<::OI::Net::SessionManager*> vWatchers = ::OI::Base::ServiceTable::GetInstance().Watchers(sNameParam);
		for (auto* pWatcher : vWatchers)
		{
			if (pWatcher != pManagerParam)
			{
				continue;
			}
			ProtoClientManager* pClient = dynamic_cast<ProtoClientManager*>(pManagerParam);
			if (pClient && pClient->IsConnect())
			{
				stRouteParam.iStatus = ServiceRouteStatus::Ok;
				stRouteParam.vClients.push_back(&pClient->Sender());
				stRouteParam.vOwners.push_back({});
				return true;
			}
		}
	}
	return false;
}

void PullRoute(const std::string& sNameParam, std::function<void(int32_t, const std::vector<::OI::Base::ServiceInstance>&)> fnDoneParam, int64_t iTimeoutMillisecondsParam)
{
	::OI::Base::RegistryClient::GetInstance().PullAsync(sNameParam, std::move(fnDoneParam), iTimeoutMillisecondsParam);
}

bool PrepareTarget(const std::string& sTargetParam, bool bBatchParam, ServiceRoute& stRouteParam, std::string& sNameParam, ServicePick& iPickParam, std::string& sZoneParam)
{
	ParsedTarget stTarget;
	if (!ParseTarget(sTargetParam, stTarget) || stTarget.bAll != bBatchParam)
	{
		stRouteParam.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	sNameParam = stTarget.sName;
	sZoneParam = stTarget.sZone;
	iPickParam = ServicePick::Any;
	if (bBatchParam)
	{
		iPickParam = ServicePick::All;
	}
	else if (!stTarget.sZone.empty())
	{
		iPickParam = ServicePick::Zone;
	}
	return false;
}

bool PrepareZones(::OI::Net::SessionManager* pManagerParam, const std::string& sNameParam, const std::vector<int32_t>& vZoneIdsParam, ServiceRoute& stRouteParam, std::vector<std::string>& vZonesParam)
{
	(void)pManagerParam;
	vZonesParam.clear();
	if (!ValidServiceToken(sNameParam) || vZoneIdsParam.empty())
	{
		stRouteParam.iStatus = ServiceRouteStatus::BadName;
		return true;
	}
	vZonesParam.reserve(vZoneIdsParam.size());
	for (int32_t iZoneId : vZoneIdsParam)
	{
		if (iZoneId < 0)
		{
			stRouteParam.iStatus = ServiceRouteStatus::BadName;
			vZonesParam.clear();
			return true;
		}
		vZonesParam.push_back(std::to_string(iZoneId));
	}
	return false;
}

ServiceRoute FinishRoute(const std::string& sNameParam, ServicePick iPickParam, const std::string& sZoneParam, int32_t iCodeParam, const std::vector<::OI::Base::ServiceInstance>& vListParam, ::OI::Net::SessionManager* pManagerParam)
{
	ServiceRoute stRoute;
	if (iCodeParam != OI_OK)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	bool bGlobal = false;
	bool bZoned = false;
	std::vector<::OI::Base::ServiceInstance> vMatched;
	for (const auto& stInst : vListParam)
	{
		if (stInst.sName != sNameParam)
		{
			continue;
		}
		if (stInst.sZone.empty())
		{
			bGlobal = true;
		}
		else
		{
			bZoned = true;
		}
		if (iPickParam == ServicePick::Zone)
		{
			if (stInst.sZone == sZoneParam)
			{
				vMatched.push_back(stInst);
			}
		}
		else
		{
			vMatched.push_back(stInst);
		}
	}
	if (bGlobal && bZoned)
	{
		stRoute.iStatus = ServiceRouteStatus::WrongKind;
		return stRoute;
	}
	if (iPickParam == ServicePick::Zone && bGlobal)
	{
		stRoute.iStatus = ServiceRouteStatus::WrongKind;
		return stRoute;
	}
	if (vMatched.empty())
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	std::sort(vMatched.begin(), vMatched.end(), [](const ::OI::Base::ServiceInstance& stLeftParam, const ::OI::Base::ServiceInstance& stRightParam)
	{
		return stLeftParam.iId < stRightParam.iId;
	});
	if (iPickParam == ServicePick::All)
	{
		for (const auto& stInst : vMatched)
		{
			if (!AddRouteClient(stRoute, stInst.sIp, stInst.sPort, pManagerParam))
			{
				return stRoute;
			}
		}
	}
	else
	{
		size_t iPick = 0;
		if (iPickParam == ServicePick::Any && vMatched.size() > 1)
		{
			static std::atomic<uint32_t> iCursor{0};
			iPick = iCursor.fetch_add(1, std::memory_order_relaxed) % vMatched.size();
		}
		const ::OI::Base::ServiceInstance& stInst = vMatched[iPick];
		// 名单已选定地址，使用固定地址的连接；订阅管理器可能在提交前改连其他分区。
		if (!AddRouteClient(stRoute, stInst.sIp, stInst.sPort, pManagerParam))
		{
			return stRoute;
		}
	}
	stRoute.iStatus = ServiceRouteStatus::Ok;
	return stRoute;
}

ServiceRoute FinishZones(const std::string& sNameParam, const std::vector<std::string>& vZonesParam, int32_t iCodeParam, const std::vector<::OI::Base::ServiceInstance>& vListParam, ::OI::Net::SessionManager* pManagerParam)
{
	ServiceRoute stRoute;
	if (iCodeParam != OI_OK)
	{
		stRoute.iStatus = ServiceRouteStatus::Offline;
		return stRoute;
	}
	bool bGlobal = false;
	for (const auto& stInst : vListParam)
	{
		if (stInst.sName != sNameParam)
		{
			continue;
		}
		if (stInst.sZone.empty())
		{
			bGlobal = true;
		}
	}
	if (bGlobal)
	{
		stRoute.iStatus = ServiceRouteStatus::WrongKind;
		return stRoute;
	}
	for (const auto& sZone : vZonesParam)
	{
		const ::OI::Base::ServiceInstance* pBest = nullptr;
		for (const auto& stInst : vListParam)
		{
			if (stInst.sName != sNameParam || stInst.sZone != sZone)
			{
				continue;
			}
			if (!pBest || stInst.iId < pBest->iId)
			{
				pBest = &stInst;
			}
		}
		if (!pBest)
		{
			stRoute.vClients.push_back(nullptr);
			stRoute.vOwners.push_back(nullptr);
			continue;
		}
		// 每个槽位固定到本次名单中的地址，不能随调用方管理器的服务切换而漂移。
		if (!AddRouteClient(stRoute, pBest->sIp, pBest->sPort, pManagerParam))
		{
			return stRoute;
		}
	}
	stRoute.iStatus = ServiceRouteStatus::Ok;
	return stRoute;
}
}
}
