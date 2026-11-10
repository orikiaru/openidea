#include "oi_rpc_peer.h"
#include "oi_rpc_coro.h"
#include "oi_rpc_session.h"
#include "oi_rpc_session_manager.h"
#include "oi_registry.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_socket.h"
#include "oi_tcp_channel.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include <cstdlib>
#include <map>
#include <tuple>
#include <utility>
namespace OI
{
namespace Rpc
{

PeerPolicy::PeerPolicy(::OI::Net::SessionManager* pManager)
{
	if (pManager)
	{
		iSendBytes = pManager->MaxSendQueueBytes();
		iReceiveBytes = pManager->MaxReceiveBufferBytes();
		iFrameBytes = pManager->MaxFrameBytes();
		iIncompleteMilliseconds = pManager->IncompleteFrameTimeoutMilliseconds();
		if (auto* pClient = dynamic_cast<ProtoClientManager*>(pManager))
		{
			std::tie(iHeldCount, iHeldBytes) = pClient->PendingCallLimits();
		}
	}
}

bool PeerPolicy::operator<(const PeerPolicy& stOther) const
{
	return std::tie(iSendBytes, iReceiveBytes, iFrameBytes, iIncompleteMilliseconds, iHeldCount, iHeldBytes) <
	       std::tie(stOther.iSendBytes, stOther.iReceiveBytes, stOther.iFrameBytes, stOther.iIncompleteMilliseconds, stOther.iHeldCount, stOther.iHeldBytes);
}

class PeerSession final : public ProtoSession
{
	std::shared_ptr<PeerConnection> m_pConnection;

  public:
	PeerSession(ProtoSessionManager* pManager, std::shared_ptr<PeerConnection> pConnection)
	    : ProtoSession(pManager), m_pConnection(std::move(pConnection))
	{
		SetFrameLimits(m_pConnection->stPolicy.iFrameBytes, m_pConnection->stPolicy.iIncompleteMilliseconds);
	}
	int32_t InstanceId() const
	{
		return m_pConnection->iInstanceId;
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

// 仅负责一次 TCP 建连，不是管理器。成功后沿用框架 TCP_Channel/ProtoSession 收发。
class PeerConnector final : public ::OI::Net::Channel
{
	std::shared_ptr<PeerConnection> m_pConnection;
	bool m_bClosed = false;

  public:
	PeerConnector(::OI::Net::NetSocket stSocket, std::shared_ptr<PeerConnection> pConnection)
	    : Channel(stSocket, nullptr), m_pConnection(std::move(pConnection))
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

sid_t ProtoSessionManager::AdoptPeer(net_socket_t iSocketLocal, const std::shared_ptr<PeerConnection>& pConnection)
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
	} stOwner{iSocketLocal};
	auto pSession = std::make_unique<PeerSession>(this, pConnection);
	::OI::Net::NetSocket stSocket;
	stSocket.Attach(iSocketLocal);
	auto pChannel = std::make_unique<::OI::Net::TCP_Channel>(stSocket, pSession.get());
	stOwner.iSocket = INVALID_SOCKET_ID; // 从这里起由通道关闭描述符。
	pSession->SetChannel(pChannel.get());
	AddSession(pSession.get(), ::OI::Net::NetAddr::Endpoint(pConnection->sIp, pConnection->sPort),
	    pConnection->stPolicy.iSendBytes, pConnection->stPolicy.iReceiveBytes);
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

std::shared_ptr<PeerConnection> PeerConnection::Create(ProtoSessionManager* pOwner, int32_t iInstanceId, std::string sIp, std::string sPort,
    PeerPolicy stPolicy, bool bPersistent, int iRetryMilliseconds)
{
	auto pConnection = std::make_shared<PeerConnection>(pOwner, iInstanceId, std::move(sIp), std::move(sPort), stPolicy, bPersistent, iRetryMilliseconds);
	pConnection->pSelf = pConnection;
	return pConnection;
}

PeerConnection::PeerConnection(ProtoSessionManager* pOwnerLocal, int32_t iInstanceIdLocal, std::string sIpLocal, std::string sPortLocal,
    PeerPolicy stPolicyLocal, bool bPersistentLocal, int iRetryMillisecondsLocal)
    : pOwner(pOwnerLocal), iInstanceId(iInstanceIdLocal), sIp(std::move(sIpLocal)), sPort(std::move(sPortLocal)), stPolicy(stPolicyLocal),
      bPersistent(bPersistentLocal), iRetryMilliseconds(iRetryMillisecondsLocal > 0 ? iRetryMillisecondsLocal : 1)
{
	stSender = {pOwner, this, [](void* pContext, uint64_t iCallId, const ::OI::Base::Bytes& stData)
	{
		static_cast<PeerConnection*>(pContext)->Submit(iCallId, stData);
	}, [](void* pContext, uint64_t iCallId)
	{
		static_cast<PeerConnection*>(pContext)->Drop(iCallId);
	}};
}

std::shared_ptr<RpcSender> PeerConnection::AcquireLease()
{
	::OI::Thread::MutexGuard stLock(stMutex);
	auto pCurrent = pLease.lock();
	if (!pCurrent)
	{
		struct Lease
		{
			std::shared_ptr<PeerConnection> pConnection;
		};
		auto pOwnerLocal = std::make_shared<Lease>(Lease{pSelf.lock()});
		pCurrent = std::shared_ptr<RpcSender>(pOwnerLocal, &stSender);
		pLease = pCurrent;
	}
	return pCurrent;
}

bool PeerConnection::HasLease()
{
	::OI::Thread::MutexGuard stLock(stMutex);
	return !pLease.expired();
}

std::shared_ptr<RpcSender> PeerConnection::SharedSender()
{
	auto pConnection = pSelf.lock();
	return pConnection ? std::shared_ptr<RpcSender>(pConnection, &pConnection->stSender) : nullptr;
}

sid_t PeerConnection::SessionId()
{
	::OI::Thread::MutexGuard stLock(stMutex);
	return iSessionId;
}

void PeerConnection::Finish(sid_t iSessionIdLocal)
{
	std::list<Held> vReady;
	bool bRetry = false;
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		bConnecting = false;
		iSessionId = iSessionIdLocal;
		vReady.swap(vHeld);
		mHeld.clear();
		iHeldBytes = 0;
		bRetry = iSessionIdLocal == INVALID_SID && bPersistent && !bRetired;
	}
	for (const auto& stCall : vReady)
	{
		if (iSessionIdLocal == INVALID_SID)
		{
			WakeRpcCall(stCall.iCallId, OI_ERR_CONNECT, nullptr);
		}
		else if (ClaimRpcSend(stCall.iCallId, iSessionIdLocal))
		{
			const int32_t iResult = pOwner->SendBytesResult(iSessionIdLocal, stCall.stData);
			if (iResult != OI_OK)
			{
				WakeRpcCall(stCall.iCallId, iResult, nullptr);
			}
		}
	}
	if (iSessionIdLocal != INVALID_SID)
	{
		pOwner->OnPeerConnect(iInstanceId, iSessionIdLocal);
	}
	else if (bRetry)
	{
		LOG_ERROR("PeerConnection connect failed id=%s instance=%d ip=%s port=%s", pOwner->GetIDString().c_str(), iInstanceId, sIp.c_str(), sPort.c_str());
		ScheduleRetry();
	}
}

void PeerConnection::Disconnected(sid_t iSessionIdLocal)
{
	bool bReconnect = false;
	{
		::OI::Thread::MutexGuard stLock(stMutex);
		if (iSessionId == iSessionIdLocal)
		{
			iSessionId = INVALID_SID;
			bReconnect = bPersistent && !bRetired;
		}
	}
	if (bReconnect && !::OI::Thread::ThreadPool::GetInstance().StopRequested())
	{
		Ensure();
	}
}

void PeerConnection::ScheduleRetry()
{
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested())
	{
		return;
	}
	std::weak_ptr<PeerConnection> pWeak = pSelf;
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([pWeak]()
	{
		if (auto pConnection = pWeak.lock())
		{
			if (!::OI::Thread::ThreadPool::GetInstance().StopRequested())
			{
				pConnection->Ensure();
			}
		}
	}, iRetryMilliseconds);
}

void PeerConnection::Drop(uint64_t iCallId)
{
	::OI::Thread::MutexGuard stLock(stMutex);
	auto it = mHeld.find(iCallId);
	if (it != mHeld.end())
	{
		iHeldBytes -= it->second->stData.Size();
		vHeld.erase(it->second);
		mHeld.erase(it);
	}
}

void PeerConnection::Submit(uint64_t iCallId, const ::OI::Base::Bytes& stData)
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
			if (!ClaimRpcSend(iCallId, iTarget))
			{
				return;
			}
		}
		else if (vHeld.size() >= stPolicy.iHeldCount || iHeldBytes > stPolicy.iHeldBytes || stData.Size() > stPolicy.iHeldBytes - iHeldBytes)
		{
			iError = OI_ERR_BUSY;
		}
		else if (MarkRpcAwaitingConnect(iCallId))
		{
			vHeld.push_back({iCallId, stData});
			try
			{
				mHeld.emplace(iCallId, std::prev(vHeld.end()));
			}
			catch (...)
			{
				vHeld.pop_back();
				throw;
			}
			iHeldBytes += stData.Size();
		}
		else
		{
			return;
		}
	}
	if (iError != OI_OK)
	{
		WakeRpcCall(iCallId, iError, nullptr);
	}
	else if (iTarget != INVALID_SID)
	{
		const int32_t iResult = pOwner->SendBytesResult(iTarget, stData);
		if (iResult != OI_OK)
		{
			WakeRpcCall(iCallId, iResult, nullptr);
		}
	}
	else
	{
		Ensure();
	}
}

void PeerConnection::Connected(net_socket_t iSocketLocal)
{
	if (iSocketLocal == INVALID_SOCKET_ID)
	{
		Finish(INVALID_SID);
		return;
	}
	sid_t iConnected = INVALID_SID;
	try
	{
		iConnected = pOwner->AdoptPeer(iSocketLocal, pSelf.lock());
	}
	catch (...)
	{
		::OI::Thread::ReportTaskError(std::current_exception());
	}
	Finish(iConnected);
}

void PeerConnection::OnResolved(void* pArgument, bool bOk, ::OI::Net::NetAddr stAddr)
{
	std::unique_ptr<std::shared_ptr<PeerConnection>> pHold(static_cast<std::shared_ptr<PeerConnection>*>(pArgument));
	if (!pHold || !*pHold)
	{
		return;
	}
	PeerConnection* pConnection = pHold->get();
	bool bRetiredLocal = false;
	{
		::OI::Thread::MutexGuard stLock(pConnection->stMutex);
		bRetiredLocal = pConnection->bRetired;
	}
	if (!bOk || bRetiredLocal)
	{
		if (!bOk)
		{
			LOG_ERROR("PeerConnection resolve failed ip=%s port=%s", pConnection->sIp.c_str(), pConnection->sPort.c_str());
		}
		pConnection->Finish(INVALID_SID);
		return;
	}
	pConnection->Connect(stAddr);
}

void PeerConnection::Begin()
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
	char* pEnd = nullptr;
	const long iPort = strtol(sPort.c_str(), &pEnd, 10);
	if (sPort.empty() || pEnd == nullptr || *pEnd || iPort < 1 || iPort > 65535)
	{
		Finish(INVALID_SID);
		return;
	}
	auto* pHold = new std::shared_ptr<PeerConnection>(pSelf.lock());
	if (!*pHold)
	{
		delete pHold;
		Finish(INVALID_SID);
		return;
	}
	::OI::Net::NetAddr::Resolve(sIp, sPort, &PeerConnection::OnResolved, pHold);
}

void PeerConnection::Connect(::OI::Net::NetAddr stAddr)
{
	::OI::Net::NetSocket stSocket;
	stSocket.Attach(stAddr.CreateTcp(true));
	if (stSocket.GetSocket() == INVALID_SOCKET_ID)
	{
		Finish(INVALID_SID);
		return;
	}
	const int iResult = NetOs_Connect(stSocket.GetSocket(), stAddr.NetAttrPtr(), stAddr.NetAttrLen());
	if (iResult == 0)
	{
		Connected(stSocket.GetSocket());
	}
	else if (errno == EINPROGRESS)
	{
		try
		{
			pConnector = new PeerConnector(stSocket, pSelf.lock());
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

void PeerConnection::Ensure()
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
		auto pRequest = std::make_unique<std::shared_ptr<PeerConnection>>(pSelf.lock());
		::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
		{
			std::unique_ptr<std::shared_ptr<PeerConnection>> pHeld(static_cast<std::shared_ptr<PeerConnection>*>(pArgument));
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

void PeerConnection::Retire()
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
		pOwner->CloseSession(iSession);
	}
	Finish(INVALID_SID);
}

// ---------------------------------------------------------------- ProtoMultiClientManager

bool ProtoMultiClientManager::Init()
{
	if (!ProtoSessionManager::Init())
	{
		return false;
	}
	std::vector<::OI::Base::ServiceInstance> vSeed;
	bool bListed = false;
	{
		::OI::Thread::MutexGuard stLock(m_stPeerLocker);
		vSeed.swap(m_vSeed);
		bListed = m_bListed;
	}
	if (bListed && !vSeed.empty())
	{
		ApplyServiceList(vSeed);
		return true;
	}
	// 没配注册或注册里还查不到时，先连配置地址，实例号 0；名单下来后按名单替换。
	ApplyPeers({{0, {GetStrIP(), GetStrPort()}}});
	return true;
}

void ProtoMultiClientManager::BindService(const std::string& sService, std::string&, std::string&)
{
	int32_t iCode = OI_ERR_FAIL;
	std::vector<::OI::Base::ServiceInstance> vList;
	const bool bPulled = ::OI::Base::RegistryClient::GetInstance().PullSync(sService, iCode, vList) && iCode == OI_OK;
	std::vector<::OI::Base::ServiceInstance> vMatched;
	for (const auto& stInst : vList)
	{
		if (stInst.sName == sService)
		{
			vMatched.push_back(stInst);
		}
	}
	::OI::Thread::MutexGuard stLock(m_stPeerLocker);
	m_bListed = bPulled;
	m_vSeed.swap(vMatched);
}

void ProtoMultiClientManager::WatchService(const std::string& sService)
{
	std::vector<::OI::Base::ServiceInstance> vInitial;
	{
		::OI::Thread::MutexGuard stLock(m_stPeerLocker);
		if (m_bListed)
		{
			vInitial = m_vSeed;
		}
	}
	::OI::Base::RegistryClient::GetInstance().WatchList(this, sService, vInitial);
}

void ProtoMultiClientManager::Start()
{
	std::vector<std::shared_ptr<PeerConnection>> vPeers;
	{
		::OI::Thread::MutexGuard stLock(m_stPeerLocker);
		m_bStarted = true;
		for (const auto& [iInstanceId, pPeer] : m_mPeers)
		{
			vPeers.push_back(pPeer);
		}
	}
	for (const auto& pPeer : vPeers)
	{
		pPeer->Ensure();
	}
}

bool ProtoMultiClientManager::UseAddress(const std::string& sIp, const std::string& sPortLocal)
{
	if (!sServiceName.empty() || sIp.empty() || sPortLocal.empty())
	{
		return false;
	}
	DefaultInit(sIp, sPortLocal, GetSessionType());
	ApplyPeers({{0, {sIp, sPortLocal}}});
	return true;
}

bool ProtoMultiClientManager::IsConnect()
{
	std::vector<std::shared_ptr<PeerConnection>> vPeers;
	{
		::OI::Thread::MutexGuard stLock(m_stPeerLocker);
		for (const auto& [iInstanceId, pPeer] : m_mPeers)
		{
			vPeers.push_back(pPeer);
		}
	}
	for (const auto& pPeer : vPeers)
	{
		if (pPeer->SessionId() != INVALID_SID)
		{
			return true;
		}
	}
	return false;
}

std::vector<int32_t> ProtoMultiClientManager::InstanceIds()
{
	std::vector<int32_t> vIds;
	::OI::Thread::MutexGuard stLock(m_stPeerLocker);
	vIds.reserve(m_mPeers.size());
	for (const auto& [iInstanceId, pPeer] : m_mPeers)
	{
		vIds.push_back(iInstanceId);
	}
	return vIds;
}

sid_t ProtoMultiClientManager::ConnectedSid(int32_t iInstanceId)
{
	std::shared_ptr<PeerConnection> pPeer;
	{
		::OI::Thread::MutexGuard stLock(m_stPeerLocker);
		auto it = m_mPeers.find(iInstanceId);
		if (it == m_mPeers.end())
		{
			return INVALID_SID;
		}
		pPeer = it->second;
	}
	return pPeer->SessionId();
}

std::shared_ptr<RpcSender> ProtoMultiClientManager::PeerSender(int32_t iInstanceId)
{
	::OI::Thread::MutexGuard stLock(m_stPeerLocker);
	auto it = m_mPeers.find(iInstanceId);
	return it == m_mPeers.end() ? nullptr : it->second->SharedSender();
}

std::shared_ptr<RpcSender> ProtoMultiClientManager::AnySender()
{
	std::vector<std::shared_ptr<PeerConnection>> vPeers;
	{
		::OI::Thread::MutexGuard stLock(m_stPeerLocker);
		vPeers.reserve(m_mPeers.size());
		for (const auto& [iInstanceId, pPeer] : m_mPeers)
		{
			vPeers.push_back(pPeer);
		}
	}
	if (vPeers.empty())
	{
		return nullptr;
	}
	std::vector<std::shared_ptr<PeerConnection>> vConnected;
	for (const auto& pPeer : vPeers)
	{
		if (pPeer->SessionId() != INVALID_SID)
		{
			vConnected.push_back(pPeer);
		}
	}
	const auto& vPick = vConnected.empty() ? vPeers : vConnected;
	const uint32_t iCursor = m_iCursor.fetch_add(1, std::memory_order_relaxed);
	return vPick[iCursor % vPick.size()]->SharedSender();
}

void ProtoMultiClientManager::AllSenders(std::vector<int32_t>& vIds, ServiceRoute& stRoute)
{
	vIds.clear();
	stRoute.vClients.clear();
	stRoute.vOwners.clear();
	stRoute.iStatus = ServiceRouteStatus::Ok;
	::OI::Thread::MutexGuard stLock(m_stPeerLocker);
	for (const auto& [iInstanceId, pPeer] : m_mPeers)
	{
		auto pSender = pPeer->SharedSender();
		vIds.push_back(iInstanceId);
		stRoute.vClients.push_back(pSender.get());
		stRoute.vOwners.push_back(std::move(pSender));
	}
}

void ProtoMultiClientManager::ApplyServiceList(const std::vector<::OI::Base::ServiceInstance>& vList)
{
	std::map<int32_t, std::pair<std::string, std::string>> mTargets;
	for (const auto& stInst : vList)
	{
		mTargets[stInst.iId] = {stInst.sIp, stInst.sPort};
	}
	ApplyPeers(std::move(mTargets));
}

void ProtoMultiClientManager::RetirePeer(const std::shared_ptr<PeerConnection>& pPeer)
{
	LOG_TRACE("peer retire %s instance=%d %s:%s", GetIDString().c_str(), pPeer->iInstanceId, pPeer->sIp.c_str(), pPeer->sPort.c_str());
	if (::OI::Net::Poll::GetInstance().InLoopThread())
	{
		pPeer->Retire();
		return;
	}
	auto pRequest = std::make_unique<std::shared_ptr<PeerConnection>>(pPeer);
	::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		std::unique_ptr<std::shared_ptr<PeerConnection>> pHeld(static_cast<std::shared_ptr<PeerConnection>*>(pArgument));
		(*pHeld)->Retire();
	}, pRequest.get());
	pRequest.release();
}

void ProtoMultiClientManager::ApplyPeers(std::map<int32_t, std::pair<std::string, std::string>> mTargets)
{
	std::vector<std::shared_ptr<PeerConnection>> vRetired;
	std::vector<std::shared_ptr<PeerConnection>> vAdded;
	bool bStarted = false;
	{
		::OI::Thread::MutexGuard stLock(m_stPeerLocker);
		bStarted = m_bStarted;
		for (auto it = m_mPeers.begin(); it != m_mPeers.end();)
		{
			auto stTarget = mTargets.find(it->first);
			if (stTarget != mTargets.end() && stTarget->second.first == it->second->sIp && stTarget->second.second == it->second->sPort)
			{
				mTargets.erase(stTarget);
				++it;
				continue;
			}
			vRetired.push_back(it->second);
			it = m_mPeers.erase(it);
		}
		for (auto& [iInstanceId, stAddress] : mTargets)
		{
			auto pPeer = PeerConnection::Create(this, iInstanceId, stAddress.first, stAddress.second, PeerPolicy(this), true, RetryMilliseconds());
			m_mPeers.emplace(iInstanceId, pPeer);
			vAdded.push_back(std::move(pPeer));
		}
	}
	// 不在表锁内结束等待：恢复的协程可能马上再次调用。
	for (const auto& pPeer : vRetired)
	{
		RetirePeer(pPeer);
	}
	for (const auto& pPeer : vAdded)
	{
		LOG_TRACE("peer add %s instance=%d %s:%s", GetIDString().c_str(), pPeer->iInstanceId, pPeer->sIp.c_str(), pPeer->sPort.c_str());
		if (bStarted)
		{
			pPeer->Ensure();
		}
	}
}

void ProtoMultiClientManager::OnDelSession(const ::OI::Net::Session* pSession)
{
	const auto* pPeerSession = dynamic_cast<const PeerSession*>(pSession);
	if (!pPeerSession)
	{
		return;
	}
	LOG_INFO("ProtoMultiClientManager::OnDelSession id=%s instance=%d sid=%d error=%d", GetIDString().c_str(), pPeerSession->InstanceId(),
	    pSession->GetSessionID(), pSession->GetError());
	OnPeerDisconnect(pPeerSession->InstanceId(), pSession->GetSessionID());
}

}
}
