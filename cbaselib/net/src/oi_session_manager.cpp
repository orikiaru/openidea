
#include "oi_config.h"
#include "oi_thread.h"
#include "oi_log.h"
#include "oi_registry.h"
#include "oi_session_manager.h"
#include "oi_session.h"
#include "oi_accept_channel.h"
#include "oi_connect_channel.h"
#include "oi_tcp_channel.h"
#include <stdio.h>
#include <vector>
#include <memory>
#include <limits>
namespace OI { namespace Net {

bool SessionManager::Init()
{
	int64_t iSendLimit = 64 * 1024 * 1024;
	TryGetConfig(GetIDString() + ".max_send_queue_bytes", iSendLimit);
	if (iSendLimit <= 0 || (uint64_t)iSendLimit > (std::numeric_limits<size_t>::max)()) return false;
	iMaxSendQueueBytes = (size_t)iSendLimit;
	std::string sIpLocal, sPortLocal, sType;
	TryGetConfigAndReturnFalse(GetIDString() + ".ip", sIpLocal);
	TryGetConfigAndReturnFalse(GetIDString() + ".port", sPortLocal);
	TryGetConfigAndReturnFalse(GetIDString() + ".type", sType);

	std::string sService;
	if (::OI::Base::Config::GlobalConfig().GetAs(GetIDString() + ".service", sService)
		&& !sService.empty())
	{
		std::string sFoundIp;
		std::string sFoundPort;
		if (::OI::Base::RegistryClient::GetInstance().Lookup(sService, sFoundIp, sFoundPort))
		{
			sIpLocal = sFoundIp;
			sPortLocal = sFoundPort;
		}
		sServiceName = sService;
	}

	assert(GetSessionTypeByString(sType) != SESSION_TYPE_INVALID);
	DefaultInit(sIpLocal, sPortLocal, GetSessionTypeByString(sType));
	sEndpoint = sIpLocal + ":" + sPortLocal;
	if (!sServiceName.empty())
	{
		::OI::Base::RegistryClient::GetInstance().Watch(this, sServiceName);
	}
	return true;
}

bool SessionManager::StartClient(bool bAsyncParam)
{
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested()) return false;
	// 把检查、建连和状态发布放在同一执行域；工人线程不直接操作连接状态。
	if (!Poll::GetInstance().InLoopThread())
	{
		if (bAsyncParam)
		{
			Poll::GetInstance().CallOnLoop([](void* pArgumentParam) {
				static_cast<SessionManager*>(pArgumentParam)->StartClient();
			}, this);
			return true;
		}
		struct ConnectRequest
		{
			SessionManager* pManager;
			::OI::Thread::Mutex stMutex;
			::OI::Thread::MutexCond stCondition;
			bool bDone = false;
			bool bResult = false;
		} stRequest{this, {}, {}, false, false};
		::OI::Thread::MutexGuard stLock(stRequest.stMutex);
		Poll::GetInstance().CallOnLoop([](void* pArgumentParam) {
			auto* pRequest = static_cast<ConnectRequest*>(pArgumentParam);
			const bool bResult = pRequest->pManager->StartClient(false);
			::OI::Thread::MutexGuard stGuard(pRequest->stMutex);
			pRequest->bResult = bResult;
			pRequest->bDone = true;
			pRequest->stCondition.Signal();
		}, &stRequest);
		while (!stRequest.bDone) stRequest.stCondition.Wait(stRequest.stMutex);
		return stRequest.bResult;
	}
	if (bServiceDown)
	{
		return false;
	}
	if (bConnectPending || HasEndpoint(sIpAddress + ":" + sPort)) return true;
	bConnectPending = true;
	const uint64_t iGeneration = ++iConnectGeneration;
	const bool bStarted = ConnectChannel::StartConnect(this, bAsyncParam);
	if (!bStarted) AbortSession(iGeneration);
	return bStarted;

}

void SessionManager::CloseEndpoint(const std::string& sEndpointParam)
{
	if (sEndpointParam.empty())
	{
		return;
	}
	std::vector<sid_t> vIds;
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		for (const auto& [iSessionId, pSession] : mSessionMap)
		{
			if (pSession->GetEndpoint() == sEndpointParam)
			{
				vIds.push_back(iSessionId);
			}
		}
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vIds.size(); ++iIndexLocal)
	{
		CloseSession(vIds[iIndexLocal]);
	}
}

bool SessionManager::HasEndpoint(const std::string& sEndpointParam)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	for (const auto& [iSessionId, pSession] : mSessionMap)
	{
		if (pSession->GetEndpoint() == sEndpointParam)
		{
			return true;
		}
	}
	return false;
}

void SessionManager::CloseAllSessions()
{
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
}

void SessionManager::ApplyService(const std::string& sIpParam, const std::string& sPortParam, bool bOnlineParam)
{
	if (!Poll::GetInstance().InLoopThread())
	{
		struct ServiceChange { SessionManager* pManager; std::string sIp; std::string sPort; bool bOnline; };
		auto pChange = std::make_unique<ServiceChange>(ServiceChange{this, sIpParam, sPortParam, bOnlineParam});
		Poll::GetInstance().CallOnLoop([](void* pArgumentParam) {
			std::unique_ptr<ServiceChange> pHeld(static_cast<ServiceChange*>(pArgumentParam));
			pHeld->pManager->ApplyService(pHeld->sIp, pHeld->sPort, pHeld->bOnline);
		}, pChange.get());
		pChange.release();
		return;
	}
	if (sServiceName.empty())
	{
		return;
	}
	if (!bOnlineParam)
	{
		if (!bServiceSeen || bServiceDown)
		{
			return;
		}
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
	std::string sEndpointLocal = sIpParam + ":" + sPortParam;
	bool bWasDown = bServiceDown;
	bool bMoved = sEndpoint != sEndpointLocal;
	bServiceSeen = true;
	if (!bWasDown && !bMoved)
	{
		return;
	}
	++iConnectGeneration;
	bConnectPending = false;
	std::string sPrevious = sEndpoint;
	bServiceDown = false;
	sEndpoint = sEndpointLocal;
	DefaultInit(sIpParam, sPortParam, iSessionType);
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

bool SessionManager::StartServer(bool bAsyncParam)
{
	if (!AcceptChannel::StartAccept(this, bAsyncParam))
	{
		return false;
	}
	return ::OI::Base::RegistryClient::GetInstance().Publish(GetIDString(), GetStrIP(), GetStrPort());
}

SESSION_TYPE SessionManager::GetSessionTypeByString(const std::string& sTypeParam)
{
	if (sTypeParam == "tcp")
	{
		return SESSION_TYPE_TCP;
	}
	return SESSION_TYPE_INVALID;
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

bool SessionManager::SendBytes(sid_t iSessionIdParam, const ::OI::Base::Bytes& stDataParam)
{
	Session* pSessionLocal = NULL;
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		auto stIterator = mSessionMap.find(iSessionIdParam);
		if (stIterator == mSessionMap.end())
		{
			return false;
		}
		// 还在表里说明 poll 线程没销毁它。持一份引用，拷贝放到管理器锁外，别的连接发送不用排队。
		pSessionLocal = stIterator->second;
		pSessionLocal->AddRef();
	}
	auto fnRelease = [](Session* pSessionParam) { pSessionParam->Release(); };
	std::unique_ptr<Session, decltype(fnRelease)> pHeld(pSessionLocal, fnRelease);
	return pSessionLocal->Send(stDataParam);
}

void SessionManager::AddSession(Session* pSessionParam)
{
	pSessionParam->SetSendQueueLimit(iMaxSendQueueBytes);
	int iSessionIdLocal = 0;
	{
		{
			::OI::Thread::MutexGuard stLock(stLocker);
			iSessionIdLocal = ++iCurrentSessionId;
			mSessionMap[iSessionIdLocal] = pSessionParam;
			pSessionParam->SetSessionID(iSessionIdLocal);
			pSessionParam->SetEndpoint(sIpAddress + ":" + sPort);
		}
		OnAddSession(pSessionParam);
	}
}

void SessionManager::AddSession(net_socket_t iNewDescriptorParam)
{
	Session* pSessionLocal = CloneSession();
	Channel* pChannelLocal = NULL;
	if (GetSessionType() == SESSION_TYPE_TCP)
	{
		NetSocket stText;
		stText.Init(iNewDescriptorParam, this);
		pChannelLocal = new TCP_Channel(stText, pSessionLocal);
	}

	if (pChannelLocal)
	{
		pSessionLocal->SetChannel(pChannelLocal);
		Poll::GetInstance().IOEventRegister(pChannelLocal, true, false);
		AddSession(pSessionLocal);
	}
	else
	{
		assert(false && "不支持的会话类型");
	}
}

void SessionManager::CompleteConnect(net_socket_t iDescriptorParam, uint64_t iGenerationParam)
{
	if (iGenerationParam != iConnectGeneration || !bConnectPending || bServiceDown)
	{
		NetOs_Close(iDescriptorParam);
		return;
	}
	bConnectPending = false;
	AddSession(iDescriptorParam);
}

void SessionManager::AbortSession(uint64_t iGenerationParam)
{
	if (iGenerationParam != iConnectGeneration || !bConnectPending) return;
	bConnectPending = false;
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		++iCurrentSessionId;
	}
	LOG_ERROR("SessionManager::AbortSession id=%s endpoint=%s", GetIDString().c_str(), sEndpoint.c_str());	OnConnectFailed();
}

void SessionManager::CloseSession(sid_t iSessionIdParam)
{
	bool bClosedLocal = false;
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		auto stIterator = mSessionMap.find(iSessionIdParam);
		if (stIterator != mSessionMap.end())
		{
			Session* pSessionLocal = stIterator->second;
			mSessionMap.erase(stIterator);
			pSessionLocal->ActiveClose();
			bClosedLocal = true;
		}
	}
	// 逻辑断开立即结束等待；唤醒放在管理器锁外。
	if (bClosedLocal) OnSessionClosed(iSessionIdParam);
}

void SessionManager::DelSession(const Session* pSessionParam)
{
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		auto stIterator = mSessionMap.find(pSessionParam->GetSessionID());
		assert(stIterator == mSessionMap.end());
	}
	//已经解绑,此时有Session锁
	OnDelSession(pSessionParam);
}
}}
