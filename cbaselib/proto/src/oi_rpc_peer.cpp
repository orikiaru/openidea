#include "oi_rpc_peer.h"
#include "oi_rpc_session_manager.h"
#include "oi_rpc_session.h"
#include "oi_registry_client.h"
#include "oi_config.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_socket.h"
#include "oi_tcp_channel.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include <cstdio>
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

PeerHeldInfo PeerConnection::HeldInfo()
{
	::OI::Thread::MutexGuard stLock(stMutex);
	return {vHeld.size(), iHeldBytes};
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
			RpcCallManager::GetInstance().Wake(stCall.iCallId, OI_ERR_CONNECT, nullptr);
		}
		else if (RpcCallManager::GetInstance().ClaimSend(stCall.iCallId, iSessionIdLocal))
		{
			const int32_t iResult = pOwner->SendBytesResult(iSessionIdLocal, stCall.stData);
			if (iResult != OI_OK)
			{
				RpcCallManager::GetInstance().Wake(stCall.iCallId, iResult, nullptr);
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
			if (!RpcCallManager::GetInstance().ClaimSend(iCallId, iTarget))
			{
				return;
			}
		}
		else if (vHeld.size() >= stPolicy.iHeldCount || iHeldBytes > stPolicy.iHeldBytes || stData.Size() > stPolicy.iHeldBytes - iHeldBytes)
		{
			iError = OI_ERR_BUSY;
		}
		else if (RpcCallManager::GetInstance().MarkAwaitingConnect(iCallId))
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
		RpcCallManager::GetInstance().Wake(iCallId, iError, nullptr);
	}
	else if (iTarget != INVALID_SID)
	{
		const int32_t iResult = pOwner->SendBytesResult(iTarget, stData);
		if (iResult != OI_OK)
		{
			RpcCallManager::GetInstance().Wake(iCallId, iResult, nullptr);
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

// ---------------------------------------------------------------- ServiceGroup

ServiceGroup::ServiceGroup(ProtoSessionManager* pOwner, bool bStarted)
    : m_pOwner(pOwner), m_bStarted(bStarted)
{
}

std::vector<std::shared_ptr<PeerConnection>> ServiceGroup::Connections()
{
	std::vector<std::shared_ptr<PeerConnection>> vConnections;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	vConnections.reserve(m_mMembers.size());
	for (const auto& [iInstanceId, stMember] : m_mMembers)
	{
		vConnections.push_back(stMember.pConnection);
	}
	return vConnections;
}

void ServiceGroup::Retire(const std::shared_ptr<PeerConnection>& pConnection)
{
	LOG_TRACE("peer retire %s instance=%d %s:%s", pConnection->pOwner->GetIDString().c_str(), pConnection->iInstanceId, pConnection->sIp.c_str(),
	    pConnection->sPort.c_str());
	if (::OI::Net::Poll::GetInstance().InLoopThread())
	{
		pConnection->Retire();
		return;
	}
	auto pRequest = std::make_unique<std::shared_ptr<PeerConnection>>(pConnection);
	::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		std::unique_ptr<std::shared_ptr<PeerConnection>> pHeld(static_cast<std::shared_ptr<PeerConnection>*>(pArgument));
		(*pHeld)->Retire();
	}, pRequest.get());
	pRequest.release();
}

void ServiceGroup::Apply(const std::vector<::OI::Base::ServiceInstance>& vList, int iRetryMilliseconds)
{
	std::map<int32_t, const ::OI::Base::ServiceInstance*> mTargets;
	for (const auto& stInstance : vList)
	{
		mTargets[stInstance.iId] = &stInstance;
	}
	std::vector<std::shared_ptr<PeerConnection>> vRetired;
	std::vector<std::shared_ptr<PeerConnection>> vAdded;
	bool bStarted = false;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		bStarted = m_bStarted;
		for (auto it = m_mMembers.begin(); it != m_mMembers.end();)
		{
			auto itTarget = mTargets.find(it->first);
			if (itTarget != mTargets.end() && itTarget->second->sIp == it->second.pConnection->sIp && itTarget->second->sPort == it->second.pConnection->sPort &&
			    itTarget->second->sZone == it->second.sZone)
			{
				mTargets.erase(itTarget);
				++it;
				continue;
			}
			vRetired.push_back(it->second.pConnection);
			it = m_mMembers.erase(it);
		}
		for (const auto& [iInstanceId, pInstance] : mTargets)
		{
			auto pConnection = PeerConnection::Create(m_pOwner, iInstanceId, pInstance->sIp, pInstance->sPort, PeerPolicy(m_pOwner), true, iRetryMilliseconds);
			m_mMembers.emplace(iInstanceId, Member{pConnection, pInstance->sZone});
			vAdded.push_back(std::move(pConnection));
		}
	}
	// 不在表锁内结束等待：恢复的协程可能马上再次调用。
	for (const auto& pConnection : vRetired)
	{
		Retire(pConnection);
	}
	for (const auto& pConnection : vAdded)
	{
		LOG_TRACE("peer add %s instance=%d %s:%s", m_pOwner->GetIDString().c_str(), pConnection->iInstanceId, pConnection->sIp.c_str(), pConnection->sPort.c_str());
		if (bStarted)
		{
			pConnection->Ensure();
		}
	}
}

void ServiceGroup::Start()
{
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		m_bStarted = true;
	}
	for (const auto& pConnection : Connections())
	{
		pConnection->Ensure();
	}
}

void ServiceGroup::RetireAll()
{
	std::map<int32_t, Member> mRetired;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		mRetired.swap(m_mMembers);
	}
	for (const auto& [iInstanceId, stMember] : mRetired)
	{
		Retire(stMember.pConnection);
	}
}

std::shared_ptr<RpcSender> ServiceGroup::Peer(int32_t iInstanceId)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	auto it = m_mMembers.find(iInstanceId);
	return it == m_mMembers.end() ? nullptr : it->second.pConnection->SharedSender();
}

std::shared_ptr<RpcSender> ServiceGroup::Any()
{
	const std::vector<std::shared_ptr<PeerConnection>> vConnections = Connections();
	// 只在已连上的里挑。一台都没连上时退到名单里，调用先暂存，连上补发。
	std::vector<std::shared_ptr<PeerConnection>> vConnected;
	for (const auto& pConnection : vConnections)
	{
		if (pConnection->SessionId() != INVALID_SID)
		{
			vConnected.push_back(pConnection);
		}
	}
	const auto& vPick = vConnected.empty() ? vConnections : vConnected;
	if (vPick.empty())
	{
		return nullptr;
	}
	const uint32_t iCursor = m_iCursor.fetch_add(1, std::memory_order_relaxed);
	return vPick[iCursor % vPick.size()]->SharedSender();
}

std::shared_ptr<RpcSender> ServiceGroup::Zone(const std::string& sZone)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (const auto& [iInstanceId, stMember] : m_mMembers)
	{
		if (stMember.sZone == sZone)
		{
			return stMember.pConnection->SharedSender();
		}
	}
	return nullptr;
}

void ServiceGroup::All(std::vector<int32_t>& vIds, ServiceRoute& stRoute)
{
	vIds.clear();
	stRoute.vClients.clear();
	stRoute.vOwners.clear();
	stRoute.iStatus = ServiceRouteStatus::Ok;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	for (const auto& [iInstanceId, stMember] : m_mMembers)
	{
		auto pSender = stMember.pConnection->SharedSender();
		vIds.push_back(iInstanceId);
		stRoute.vClients.push_back(pSender.get());
		stRoute.vOwners.push_back(std::move(pSender));
	}
}

std::vector<int32_t> ServiceGroup::Ids()
{
	std::vector<int32_t> vIds;
	::OI::Thread::MutexGuard stLock(m_stMutex);
	vIds.reserve(m_mMembers.size());
	for (const auto& [iInstanceId, stMember] : m_mMembers)
	{
		vIds.push_back(iInstanceId);
	}
	return vIds;
}

sid_t ServiceGroup::SessionId(int32_t iInstanceId)
{
	std::shared_ptr<PeerConnection> pConnection;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		auto it = m_mMembers.find(iInstanceId);
		if (it == m_mMembers.end())
		{
			return INVALID_SID;
		}
		pConnection = it->second.pConnection;
	}
	return pConnection->SessionId();
}

sid_t ServiceGroup::ConnectedAny()
{
	std::vector<sid_t> vSids;
	for (const auto& pConnection : Connections())
	{
		const sid_t iSessionIdLocal = pConnection->SessionId();
		if (iSessionIdLocal != INVALID_SID)
		{
			vSids.push_back(iSessionIdLocal);
		}
	}
	if (vSids.empty())
	{
		return INVALID_SID;
	}
	const uint32_t iCursor = m_iCursor.fetch_add(1, std::memory_order_relaxed);
	return vSids[iCursor % vSids.size()];
}

PeerHeldInfo ServiceGroup::HeldInfo(int32_t iInstanceId)
{
	std::shared_ptr<PeerConnection> pConnection;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		auto it = m_mMembers.find(iInstanceId);
		if (it == m_mMembers.end())
		{
			return {};
		}
		pConnection = it->second.pConnection;
	}
	return pConnection->HeldInfo();
}

size_t ServiceGroup::Size()
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	return m_mMembers.size();
}

// ---------------------------------------------------------------- ProtoMultiClientManager

ProtoMultiClientManager::ProtoMultiClientManager()
    : m_stGroup(this, false)
{
}

bool ProtoMultiClientManager::AddressOptional()
{
	std::vector<std::string> vLines;
	return ::OI::Base::Config::GlobalConfig().GetArrayAs(GetIDString() + ".instances", vLines);
}

int32_t ProtoMultiClientManager::LoadInstances(std::vector<::OI::Base::ServiceInstance>& vList)
{
	vList.clear();
	std::vector<std::string> vLines;
	if (!::OI::Base::Config::GlobalConfig().GetArrayAs(GetIDString() + ".instances", vLines))
	{
		::OI::Base::ServiceInstance stInstance;
		stInstance.sIp = GetStrIP();
		stInstance.sPort = GetStrPort();
		vList.push_back(std::move(stInstance));
		return OI_OK;
	}
	for (const std::string& sLine : vLines)
	{
		::OI::Base::ServiceInstance stInstance;
		bool bValid = ::OI::Base::StaticRegistry::ParseLine(sLine, false, stInstance);
		for (const auto& stHave : vList)
		{
			bValid = bValid && stHave.iId != stInstance.iId;
		}
		if (!bValid)
		{
			fprintf(stderr, "%s bad instance \"%s\"\n", GetIDString().c_str(), sLine.c_str());
			fflush(stderr);
			LOG_ERROR("%s bad instance", GetIDString().c_str());
			return OI_ERR_INVALID_ARGUMENT;
		}
		vList.push_back(std::move(stInstance));
	}
	if (vList.empty())
	{
		fprintf(stderr, "%s instances empty\n", GetIDString().c_str());
		fflush(stderr);
		return OI_ERR_INVALID_ARGUMENT;
	}
	return OI_OK;
}

int32_t ProtoMultiClientManager::Init()
{
	int32_t iCode = ProtoSessionManager::Init();
	if (iCode != OI_OK)
	{
		return iCode;
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
		return OI_OK;
	}
	// 没配注册或注册里还查不到时，先连配置地址：instances 按配置的实例号，只有 ip/port 时实例号 0；名单下来后按名单替换。
	iCode = LoadInstances(vSeed);
	if (iCode != OI_OK)
	{
		return iCode;
	}
	m_stGroup.Apply(vSeed, RetryMilliseconds());
	return OI_OK;
}

void ProtoMultiClientManager::BindService(const std::string& sService, std::string&, std::string&)
{
	std::vector<::OI::Base::ServiceInstance> vList;
	const int32_t iPullStatus = ::OI::Base::RegistryClient::GetInstance().PullSync(sService, vList);
	std::vector<::OI::Base::ServiceInstance> vMatched;
	for (const auto& stInst : vList)
	{
		if (stInst.sName == sService)
		{
			vMatched.push_back(stInst);
		}
	}
	::OI::Thread::MutexGuard stLock(m_stPeerLocker);
	m_bListed = iPullStatus == OI_OK;
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

int32_t ProtoMultiClientManager::Start()
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::Start");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	m_stGroup.Start();
	return OI_OK;
}

int32_t ProtoMultiClientManager::UseAddress(const std::string& sIp, const std::string& sPortLocal)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::UseAddress");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	if (!sServiceName.empty() || sIp.empty() || sPortLocal.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	DefaultInit(sIp, sPortLocal, GetSessionType());
	::OI::Base::ServiceInstance stInstance;
	stInstance.sIp = sIp;
	stInstance.sPort = sPortLocal;
	m_stGroup.Apply({stInstance}, RetryMilliseconds());
	return OI_OK;
}

bool ProtoMultiClientManager::IsConnect()
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::IsConnect") != OI_OK)
	{
		return false;
	}

	return m_stGroup.ConnectedAny() != INVALID_SID;
}

std::vector<int32_t> ProtoMultiClientManager::InstanceIds()
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::InstanceIds") != OI_OK)
	{
		return {};
	}

	return m_stGroup.Ids();
}

sid_t ProtoMultiClientManager::ConnectedSid(int32_t iInstanceId)
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::ConnectedSid") != OI_OK)
	{
		return INVALID_SID;
	}

	return m_stGroup.SessionId(iInstanceId);
}

PeerHeldInfo ProtoMultiClientManager::HeldInfo(int32_t iInstanceId)
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::HeldInfo") != OI_OK)
	{
		return {};
	}

	return m_stGroup.HeldInfo(iInstanceId);
}

std::shared_ptr<RpcSender> ProtoMultiClientManager::PeerSender(int32_t iInstanceId)
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::PeerSender") != OI_OK)
	{
		return {};
	}

	return m_stGroup.Peer(iInstanceId);
}

std::shared_ptr<RpcSender> ProtoMultiClientManager::AnySender()
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::AnySender") != OI_OK)
	{
		return {};
	}

	return m_stGroup.Any();
}

sid_t ProtoMultiClientManager::ConnectedAny()
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::ConnectedAny") != OI_OK)
	{
		return INVALID_SID;
	}

	return m_stGroup.ConnectedAny();
}

void ProtoMultiClientManager::AllSenders(std::vector<int32_t>& vIds, ServiceRoute& stRoute)
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::AllSenders") != OI_OK)
	{
		return;
	}

	m_stGroup.All(vIds, stRoute);
}

void ProtoMultiClientManager::ManySenders(const std::vector<int32_t>& vInstanceIds, ServiceRoute& stRoute)
{
	stRoute.vClients.clear();
	stRoute.vOwners.clear();
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoMultiClientManager::ManySenders") != OI_OK)
	{
		return;
	}

	stRoute.iStatus = ServiceRouteStatus::Ok;
	for (int32_t iInstanceId : vInstanceIds)
	{
		auto pSender = m_stGroup.Peer(iInstanceId);
		stRoute.vClients.push_back(pSender.get());
		stRoute.vOwners.push_back(std::move(pSender));
	}
}

void ProtoMultiClientManager::ApplyServiceList(const std::vector<::OI::Base::ServiceInstance>& vList)
{
	m_stGroup.Apply(vList, RetryMilliseconds());
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
