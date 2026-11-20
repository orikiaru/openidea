
#include "oi_config.h"
#include "oi_thread.h"
#include "oi_log.h"
#include "oi_registry_client.h"
#include "oi_session_manager.h"
#include "oi_session.h"
#include "oi_accept_channel.h"
#include "oi_connect_channel.h"
#include "oi_tcp_channel.h"
#include "oi_udp_channel.h"
#include <stdio.h>
#include <vector>
#include <memory>
#include <limits>
#include <stdexcept>
namespace OI
{
namespace Net
{

int32_t SessionManager::LoadSessionLimits()
{
	int64_t iSendLimit = 64 * 1024 * 1024;
	TryGetConfig(GetIDString() + ".max_send_queue_bytes", iSendLimit);
	if (iSendLimit <= 0 || (uint64_t)iSendLimit > (std::numeric_limits<size_t>::max)())
	{
		fprintf(stderr, "invalid send limit for %s\n", GetIDString().c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	int64_t iReceiveLimit = 64 * 1024 * 1024 + 8;
	int64_t iFrameLimit = 64 * 1024 * 1024;
	int64_t iFrameTimeout = 10000;
	int64_t iIdleTimeout = 60000;
	TryGetConfig(GetIDString() + ".max_receive_buffer_bytes", iReceiveLimit);
	TryGetConfig(GetIDString() + ".max_frame_bytes", iFrameLimit);
	TryGetConfig(GetIDString() + ".incomplete_frame_timeout_ms", iFrameTimeout);
	TryGetConfig(GetIDString() + ".idle_timeout_ms", iIdleTimeout);
	if (iFrameLimit < 8 || iFrameLimit > INT32_MAX || iReceiveLimit < iFrameLimit + 8 ||
	    static_cast<uint64_t>(iReceiveLimit) > SIZE_MAX || iFrameTimeout <= 0 || iIdleTimeout <= 0)
	{
		fprintf(stderr, "invalid receive limits for %s\n", GetIDString().c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	iMaxSendQueueBytes = static_cast<size_t>(iSendLimit);
	iMaxReceiveBufferBytes = static_cast<size_t>(iReceiveLimit);
	iMaxFrameBytes = static_cast<size_t>(iFrameLimit);
	iIncompleteFrameTimeoutMilliseconds = static_cast<uint64_t>(iFrameTimeout);
	iIdleTimeoutMilliseconds = static_cast<uint64_t>(iIdleTimeout);
	return OI_OK;
}

int32_t SessionManager::Init()
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("SessionManager::Init");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	const int32_t iLimitStatus = LoadSessionLimits();
	if (iLimitStatus != OI_OK)
	{
		return iLimitStatus;
	}
	std::string sIpLocal, sPortLocal, sType;
	const bool bHasIp = TryGetConfig(GetIDString() + ".ip", sIpLocal);
	const bool bHasPort = TryGetConfig(GetIDString() + ".port", sPortLocal);
	if ((!bHasIp || !bHasPort) && !AddressOptional())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (!TryGetConfig(GetIDString() + ".type", sType))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	std::string sService;
	if (::OI::Base::Config::GlobalConfig().GetAs(GetIDString() + ".service", sService) && !sService.empty())
	{
		BindService(sService, sIpLocal, sPortLocal);
		const int32_t iRegistryStatus = ::OI::Base::RegistryClient::GetInstance().ConfigStatus();
		if (iRegistryStatus != OI_OK)
		{
			fprintf(stderr, "SessionManager::Init registry service=%s code=%d\n", sService.c_str(), iRegistryStatus);
			fflush(stderr);
			return iRegistryStatus;
		}
		sServiceName = sService;
	}

	const SESSION_TYPE iConfiguredType = GetSessionTypeByString(sType);
	if (iConfiguredType == SESSION_TYPE_INVALID)
	{
		fprintf(stderr, "SessionManager::Init invalid session type id=%s code=%d\n",
		    GetIDString().c_str(), OI_ERR_INVALID_ARGUMENT);
		return OI_ERR_INVALID_ARGUMENT;
	}
	DefaultInit(sIpLocal, sPortLocal, iConfiguredType);
	if (!sServiceName.empty())
	{
		WatchService(sServiceName);
	}
	return OI_OK;
}

void SessionManager::BindService(const std::string& sService, std::string& sIpLocal, std::string& sPortLocal)
{
	std::string sFoundIp;
	std::string sFoundPort;
	if (::OI::Base::RegistryClient::GetInstance().Lookup(sService, sFoundIp, sFoundPort) == OI_OK)
	{
		sIpLocal = sFoundIp;
		sPortLocal = sFoundPort;
	}
}

void SessionManager::WatchService(const std::string& sService)
{
	::OI::Base::RegistryClient::GetInstance().Watch(this, sService);
}

struct SessionManager::ConnectWait
{
	std::atomic<OI::Coro::Gate> iGate{OI::Coro::Gate::Idle};
	std::coroutine_handle<> stHandle;
	int32_t iResult = OI_ERR_CONNECT;
	void Finish(int32_t iCode)
	{
		iResult = iCode;
		if (iGate.exchange(OI::Coro::Gate::Ready) == OI::Coro::Gate::Waiting)
		{
			OI::Coro::Schedule(stHandle);
		}
	}
	bool await_ready() const noexcept
	{
		return iGate.load() == OI::Coro::Gate::Ready;
	}
	bool await_suspend(std::coroutine_handle<> stParent) noexcept
	{
		stHandle = stParent;
		return iGate.exchange(OI::Coro::Gate::Waiting) != OI::Coro::Gate::Ready;
	}
	int32_t await_resume() const noexcept
	{
		return iResult;
	}
};

OI::Coro::TaskResult<int32_t> SessionManager::WaitForConnect(std::shared_ptr<ConnectWait> pWait)
{
	co_return co_await *pWait;
}

OI::Coro::TaskResult<int32_t> SessionManager::StartClient()
{
	auto pWait = std::make_shared<ConnectWait>();
	auto stResult = WaitForConnect(pWait);
	BeginConnect(pWait);
	return stResult;
}

void SessionManager::FinishConnectWaiters(int32_t iResultLocal)
{
	auto vWaiters = std::move(m_vConnectWaiters);
	m_vConnectWaiters.clear();
	for (const auto& pWait : vWaiters)
	{
		pWait->Finish(iResultLocal);
	}
}

void SessionManager::BeginConnect(const std::shared_ptr<ConnectWait>& pWaitLocal)
{
	if (OI::Thread::ThreadPool::GetInstance().StopRequested())
	{
		pWaitLocal->Finish(OI_ERR_CONNECT);
		return;
	}
	if (Poll::GetInstance().InLoopThread())
	{
		StartConnect(pWaitLocal);
		return;
	}
	struct ConnectRequest
	{
		SessionManager* pManager;
		std::shared_ptr<ConnectWait> pWait;
	};
	auto pRequest = std::make_unique<ConnectRequest>(ConnectRequest{this, pWaitLocal});
	Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		std::unique_ptr<ConnectRequest> pHeld(static_cast<ConnectRequest*>(pArgument));
		pHeld->pManager->StartConnect(pHeld->pWait);
	}, pRequest.get());
	pRequest.release();
}

void SessionManager::StartConnect(const std::shared_ptr<ConnectWait>& pWaitLocal)
{
	assert(Poll::GetInstance().InLoopThread());
	if (OI::Thread::ThreadPool::GetInstance().StopRequested() || bServiceDown)
	{
		pWaitLocal->Finish(OI_ERR_CONNECT);
		return;
	}
	if (GetSessionType() == SESSION_TYPE_UDP)
	{
		pWaitLocal->Finish(pUdpChannel ? OI_OK : UDP_Channel::StartBind(this));
		return;
	}
	if (GetSessionType() != SESSION_TYPE_TCP)
	{
		pWaitLocal->Finish(OI_ERR_INVALID_ARGUMENT);
		return;
	}
	if (!bConnectPending && HasEndpoint(sEndpoint))
	{
		pWaitLocal->Finish(OI_OK);
		return;
	}
	// 当前列表只属于当前未完成的代次。必须先登记，再启动可能立即完成的 connect。
	m_vConnectWaiters.push_back(pWaitLocal);
	if (bConnectPending)
	{
		return;
	}
	bConnectPending = true;
	struct ConnectResolve
	{
		SessionManager* pManager;
		uint64_t iGeneration;
	};
	auto* pJob = new ConnectResolve{this, ++iConnectGeneration};
	NetAddr::Resolve(sIpAddress, sPort, [](void* pArgument, bool bOk, NetAddr stAddr)
	{
		std::unique_ptr<ConnectResolve> pResolved(static_cast<ConnectResolve*>(pArgument));
		SessionManager* pManager = pResolved->pManager;
		// 代次已经被换掉或收过口时，等待者由那边唤醒。这一代还挂着就必须走 AbortSession，co_await StartClient() 才能回来。
		if (pResolved->iGeneration != pManager->iConnectGeneration || !pManager->bConnectPending)
		{
			return;
		}
		if (pManager->bServiceDown || !bOk || stAddr.Host().empty())
		{
			pManager->AbortSession(pResolved->iGeneration);
			return;
		}
		pManager->sEndpoint = stAddr.Endpoint();
		if (pManager->HasEndpoint(pManager->sEndpoint))
		{
			pManager->bConnectPending = false;
			pManager->FinishConnectWaiters(OI_OK);
			return;
		}
		const int32_t iConnectStatus = ConnectChannel::StartConnect(pManager, stAddr);
		if (iConnectStatus != OI_OK)
		{
			pManager->AbortSession(pResolved->iGeneration, iConnectStatus);
		}
	}, pJob);
}

int32_t SessionManager::CloseEndpoint(const std::string& sEndpoint)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("SessionManager::CloseEndpoint");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (sEndpoint.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::vector<sid_t> vIds;
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		for (const auto& [iSessionId, pSession] : mSessionMap)
		{
			if (pSession->GetEndpoint() == sEndpoint)
			{
				vIds.push_back(iSessionId);
			}
		}
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vIds.size(); ++iIndexLocal)
	{
		CloseSession(vIds[iIndexLocal]);
	}
	return vIds.empty() ? OI_ERR_NOT_FOUND : OI_OK;
}

bool SessionManager::HasEndpoint(const std::string& sEndpoint)
{
	if (Poll::GetInstance().CheckThreadAccess("SessionManager::HasEndpoint") != OI_OK)
	{
		return false;
	}

	::OI::Thread::MutexGuard stLock(stLocker);
	for (const auto& [iSessionId, pSession] : mSessionMap)
	{
		if (pSession->GetEndpoint() == sEndpoint)
		{
			return true;
		}
	}
	return false;
}

int32_t SessionManager::CloseAllSessions()
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("SessionManager::CloseAllSessions");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	std::vector<sid_t> vIds;
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		for (const auto& [iSessionId, pSession] : mSessionMap)
		{
			vIds.push_back(iSessionId);
		}
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vIds.size(); ++iIndexLocal)
	{
		CloseSession(vIds[iIndexLocal]);
	}
	return OI_OK;
}

void SessionManager::ApplyService(const std::string& sIpLocal, const std::string& sPortLocal, bool bOnlineLocal)
{
	if (!Poll::GetInstance().InLoopThread())
	{
		struct ServiceChange
		{
			SessionManager* pManager;
			std::string sIp;
			std::string sPort;
			bool bOnline;
		};
		auto pChange = std::make_unique<ServiceChange>(ServiceChange{this, sIpLocal, sPortLocal, bOnlineLocal});
		Poll::GetInstance().CallOnLoop([](void* pArgument)
		{
			std::unique_ptr<ServiceChange> pHeld(static_cast<ServiceChange*>(pArgument));
			pHeld->pManager->ApplyService(pHeld->sIp, pHeld->sPort, pHeld->bOnline);
		}, pChange.get());
		pChange.release();
		return;
	}
	if (sServiceName.empty())
	{
		return;
	}
	if (!bOnlineLocal)
	{
		if (!bServiceSeen || bServiceDown)
		{
			return;
		}
		FinishConnectWaiters(OI_ERR_CONNECT);
		++iConnectGeneration;
		bConnectPending = false;
		bServiceDown = true;
		std::string sPrevious = sEndpoint;
		sEndpoint.clear();
		LOG_TRACE("service offline %s", sServiceName.c_str());
		fflush(stdout);
		CloseEndpoint(sPrevious);
		OnServiceDown();
		return;
	}
	std::string sEndpointLocal = NetAddr::Endpoint(sIpLocal, sPortLocal);
	bool bWasDown = bServiceDown;
	bool bMoved = NetAddr::Endpoint(sIpAddress, sPort) != sEndpointLocal;
	bServiceSeen = true;
	if (!bWasDown && !bMoved)
	{
		return;
	}
	FinishConnectWaiters(OI_ERR_CONNECT);
	++iConnectGeneration;
	bConnectPending = false;
	std::string sPrevious = sEndpoint;
	bServiceDown = false;
	DefaultInit(sIpLocal, sPortLocal, iSessionType);
	LOG_TRACE("service switch %s %s -> %s", sServiceName.c_str(), sPrevious.c_str(), sEndpointLocal.c_str());
	fflush(stdout);
	if (!sPrevious.empty() && sPrevious != sEndpointLocal)
	{
		CloseEndpoint(sPrevious);
	}
	if (!HasEndpoint(sEndpointLocal))
	{
		StartClient();
	}
}

int32_t SessionManager::StartServer(bool bAsync)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("SessionManager::StartServer");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!bAsync || iSessionType == SESSION_TYPE_INVALID)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	const int32_t iAcceptStatus = AcceptChannel::StartAccept(this, bAsync);
	if (iAcceptStatus != OI_OK)
	{
		return iAcceptStatus;
	}
	// 注册表登记的是 RPC 连接地址，UDP 口不登记。
	if (iSessionType == SESSION_TYPE_UDP)
	{
		return OI_OK;
	}
	return ::OI::Base::RegistryClient::GetInstance().Publish(GetIDString(), GetStrIP(), GetStrPort());
}

SESSION_TYPE SessionManager::GetSessionTypeByString(const std::string& sType)
{
	if (sType == "tcp")
	{
		return SESSION_TYPE_TCP;
	}
	if (sType == "udp")
	{
		return SESSION_TYPE_UDP;
	}
	if (sType == "fifo")
	{
		return SESSION_TYPE_FIFO;
	}
	return SESSION_TYPE_INVALID;
}

uint16_t SessionManager::LocalPort() const
{
	return pUdpChannel ? pUdpChannel->LocalPort() : 0;
}

Session* SessionManager::OpenUdpPeerLocked(const NetAddr& stAddr)
{
	const std::string sEndpoint = stAddr.Endpoint();
	auto it = m_mUdpPeers.find(sEndpoint);
	if (it != m_mUdpPeers.end() && !it->second->pSession->IsClosed())
	{
		return it->second->pSession;
	}
	// 旧会话关了还没回收时，新会话顶掉表项；回收只摘自己那一项。
	const net_socket_t iDescriptor = NetOs_Dup(pUdpChannel->stSocket.GetSocket());
	if (iDescriptor == INVALID_SOCKET_ID)
	{
		LOG_ERROR("UDP dup peer=%s err=%s", sEndpoint.c_str(), NetOs_StrError(errno));
		return nullptr;
	}
	NetSocket stSocketLocal;
	stSocketLocal.Attach(iDescriptor);
	Session* pSessionLocal = CloneSession();
	pSessionLocal->stPeerAddr = stAddr;
	pSessionLocal->iPeerActive.store(::OI::Timer::SteadyMilliseconds(), std::memory_order_relaxed);
	auto* pChannel = new UDP_Channel(stSocketLocal, this, pUdpChannel->LocalPort(), pSessionLocal);
	pSessionLocal->SetChannel(pChannel);
	m_mUdpPeers[sEndpoint] = pChannel;
	RegisterSession(pSessionLocal, sEndpoint, MaxSendQueueBytes(), MaxReceiveBufferBytes());
	OnAddSession(pSessionLocal);
	return pSessionLocal;
}

void SessionManager::RecycleUdpPeer(Session* pSessionLocal)
{
	{
		::OI::Thread::MutexGuard stLock(m_stUdpPeerLock);
		auto it = m_mUdpPeers.find(pSessionLocal->GetEndpoint());
		if (it != m_mUdpPeers.end() && it->second->pSession == pSessionLocal)
		{
			m_mUdpPeers.erase(it);
		}
	}
	pSessionLocal->DetachChannel();
	pSessionLocal->OnClose();
	pSessionLocal->Release();
}

void SessionManager::CloseIdleUdpPeers()
{
	const uint64_t iNow = ::OI::Timer::SteadyMilliseconds();
	const uint64_t iIdle = IdleTimeoutMilliseconds();
	std::vector<Session*> vIdle;
	{
		::OI::Thread::MutexGuard stLock(m_stUdpPeerLock);
		for (const auto& [sEndpoint, pChannel] : m_mUdpPeers)
		{
			Session* pSessionLocal = pChannel->pSession;
			const uint64_t iLast = pSessionLocal->iPeerActive.load(std::memory_order_relaxed);
			if (iNow > iLast && iNow - iLast >= iIdle && !pSessionLocal->IsClosed())
			{
				vIdle.push_back(pSessionLocal);
			}
		}
	}
	// 关闭会回调管理器，出锁再关。
	// 会话只在 poll 回调里回收，本轮结束前候选指针有效。
	for (Session* pSessionLocal : vIdle)
	{
		{
			::OI::Thread::MutexGuard stLock(pSessionLocal->stLocker);
			const uint64_t iLast = pSessionLocal->iPeerActive.load(std::memory_order_relaxed);
			if (iNow <= iLast || iNow - iLast < iIdle || pSessionLocal->IsClosed())
			{
				continue;
			}
			pSessionLocal->bPeerIdleExpired = true;
		}
		pSessionLocal->PassiveClose(ETIMEDOUT);
	}
}

int32_t SessionManager::OpenPeer(const std::string& sIp, const std::string& sPortLocal, sid_t& iSessionId)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("SessionManager::OpenPeer");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pUdpChannel)
	{
		return OI_ERR_CONNECT;
	}
	NetAddr stAddr;
	if (!stAddr.Init(sIp, sPortLocal))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	::OI::Thread::MutexGuard stLock(m_stUdpPeerLock);
	Session* pPeer = OpenUdpPeerLocked(stAddr);
	if (!pPeer)
	{
		return OI_ERR_SOCKET;
	}
	iSessionId = pPeer->GetSessionID();
	return OI_OK;
}

int32_t SessionManager::GetPeerAddr(sid_t iSessionId, NetAddr& stAddr)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("SessionManager::GetPeerAddr");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pUdpChannel)
	{
		return OI_ERR_CONNECT;
	}
	::OI::Thread::MutexGuard stLock(stLocker);
	auto it = mSessionMap.find(iSessionId);
	if (it == mSessionMap.end())
	{
		return OI_ERR_NOT_FOUND;
	}
	stAddr = it->second->stPeerAddr;
	return OI_OK;
}

SessionManager::~SessionManager()
{
	::OI::Thread::MutexGuard stLock(stLocker);
	for (const auto& [iSessionId, pSession] : mSessionMap)
	{
		Session* pSessionLocal = pSession;
		pSessionLocal->ActiveClose();
	}
	mSessionMap.clear();
}

const std::string SessionManager::GetIDString()
{
	return "SessionManager";
}

std::shared_ptr<void> SessionManager::HoldSessionTask(sid_t iSessionId)
{
	if (Poll::GetInstance().CheckThreadAccess("SessionManager::HoldSessionTask") != OI_OK)
	{
		return {};
	}

	::OI::Thread::MutexGuard stLock(stLocker);
	auto it = mSessionMap.find(iSessionId);
	return it == mSessionMap.end() ? nullptr : it->second->HoldTask();
}

int32_t SessionManager::SendBytes(sid_t iSessionId, const ::OI::Base::Bytes& stData)
{
	return SendBytesResult(iSessionId, stData);
}

int32_t SessionManager::SendBytesResult(sid_t iSessionId, const ::OI::Base::Bytes& stData)
{
	return SendToSession(iSessionId, stData, false);
}

int32_t SessionManager::SendFrame(sid_t iSessionId, const ::OI::Base::Bytes& stData)
{
	return SendToSession(iSessionId, stData, true);
}

int32_t SessionManager::SendToSession(sid_t iSessionId, const ::OI::Base::Bytes& stData, bool bFramed)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("SessionManager::SendBytesResult");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	Session* pSessionLocal = NULL;
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		auto stIterator = mSessionMap.find(iSessionId);
		if (stIterator == mSessionMap.end())
		{
			return OI_ERR_CONNECT;
		}
		// 还在表里说明 poll 线程没销毁它。持一份引用，拷贝放到管理器锁外，别的连接发送不用排队。
		pSessionLocal = stIterator->second;
		pSessionLocal->AddRef();
	}
	auto fnRelease = [](Session* pSession)
	{
		pSession->Release();
	};
	std::unique_ptr<Session, decltype(fnRelease)> pHeld(pSessionLocal, fnRelease);
	if (bFramed && pSessionLocal->stPeerAddr.IsValidAttr())
	{
		return pSessionLocal->SendBuffered(stData, true);
	}
	return pSessionLocal->SendResult(stData);
}

void SessionManager::AddSession(Session* pSession)
{
	AddSession(pSession, sEndpoint, iMaxSendQueueBytes, iMaxReceiveBufferBytes);
}

void SessionManager::AddSession(Session* pSession, const std::string& sEndpoint, size_t iSendBytes, size_t iReceiveBytes)
{
	RegisterSession(pSession, sEndpoint, iSendBytes, iReceiveBytes);
	OnAddSession(pSession);
}

void SessionManager::RegisterSession(Session* pSession, const std::string& sEndpoint, size_t iSendBytes, size_t iReceiveBytes)
{
	pSession->SetSendQueueLimit(iSendBytes);
	pSession->SetReceiveQueueLimit(iReceiveBytes);
	pSession->SetEndpoint(sEndpoint);
	::OI::Thread::MutexGuard stLock(stLocker);
	const sid_t iSessionIdLocal = ++iCurrentSessionId;
	mSessionMap[iSessionIdLocal] = pSession;
	pSession->SetSessionID(iSessionIdLocal);
}

int32_t SessionManager::AddSession(net_socket_t iNewDescriptor)
{
	Session* pSessionLocal = CloneSession();
	Channel* pChannelLocal = NULL;
	if (GetSessionType() == SESSION_TYPE_TCP)
	{
		NetSocket stText;
		stText.Attach(iNewDescriptor);
		pChannelLocal = new TCP_Channel(stText, pSessionLocal);
	}

	if (!pChannelLocal)
	{
		assert(false && "不支持的会话类型");
		NetOs_Close(iNewDescriptor);
		pSessionLocal->Release();
		return OI_ERR_INVALID_ARGUMENT;
	}
	pSessionLocal->SetChannel(pChannelLocal);
	const int32_t iRegisterStatus = Poll::GetInstance().IOEventRegister(pChannelLocal, true, false);
	if (iRegisterStatus != OI_OK)
	{
		LOG_ERROR("SessionManager::AddSession poll register failed id=%s", GetIDString().c_str());
		pSessionLocal->DetachChannel();
		delete pChannelLocal;
		pSessionLocal->Release();
		return iRegisterStatus;
	}
	AddSession(pSessionLocal);
	return OI_OK;
}

void SessionManager::CompleteConnect(net_socket_t iDescriptor, uint64_t iGeneration)
{
	if (iGeneration != iConnectGeneration || !bConnectPending || bServiceDown)
	{
		NetOs_Close(iDescriptor);
		return;
	}
	// OnAddSession 可以重入并发起下一代连接，不能在它返回后取走当前列表。
	auto vWaiters = std::move(m_vConnectWaiters);
	m_vConnectWaiters.clear();
	bConnectPending = false;
	const int32_t iStatus = AddSession(iDescriptor);
	for (const auto& pWait : vWaiters)
	{
		pWait->Finish(iStatus);
	}
	if (iStatus != OI_OK)
	{
		OnConnectFailed();
	}
}

void SessionManager::CancelConnect()
{
	if (!bConnectPending)
	{
		return;
	}
	++iConnectGeneration;
	bConnectPending = false;
	FinishConnectWaiters(OI_ERR_CONNECT);
}

void SessionManager::AbortSession(uint64_t iGeneration, int32_t iCode)
{
	if (iGeneration != iConnectGeneration || !bConnectPending)
	{
		return;
	}
	bConnectPending = false;
	FinishConnectWaiters(iCode);
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		++iCurrentSessionId;
	}
	LOG_ERROR("SessionManager::AbortSession id=%s endpoint=%s", GetIDString().c_str(), sEndpoint.c_str());
	OnConnectFailed();
}

int32_t SessionManager::CloseSession(sid_t iSessionId)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("SessionManager::CloseSession");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	bool bClosedLocal = false;
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		auto stIterator = mSessionMap.find(iSessionId);
		if (stIterator != mSessionMap.end())
		{
			Session* pSessionLocal = stIterator->second;
			mSessionMap.erase(stIterator);
			pSessionLocal->ActiveClose();
			bClosedLocal = true;
		}
	}
	// 逻辑断开立即结束等待；唤醒放在管理器锁外。
	if (bClosedLocal)
	{
		OnSessionClosed(iSessionId);
	}
	return bClosedLocal ? OI_OK : OI_ERR_NOT_FOUND;
}

void SessionManager::DelSession(const Session* pSession)
{
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		[[maybe_unused]] auto stIterator = mSessionMap.find(pSession->GetSessionID());
		assert(stIterator == mSessionMap.end());
	}
	//已经解绑,此时有Session锁
	OnDelSession(pSession);
}
}
}
