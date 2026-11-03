
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

struct SessionManager::ConnectWait
{
	OI::Thread::Mutex stMutex;
	bool bDone = false;
	bool bResult = false;
	std::atomic<int> iGate{0};
	std::coroutine_handle<> stHandle;
	void (*fnSchedule)(std::coroutine_handle<>) = nullptr;
	void Finish(bool bResultParam)
	{
		{
			OI::Thread::MutexGuard stLock(stMutex);
			if (bDone) return;
			bResult = bResultParam;
			bDone = true;
		}
		// 回调先到只发布结果；调用方已挂起才排队恢复，禁止直接 resume。
		if (iGate.exchange(1, std::memory_order_acq_rel) == 2) fnSchedule(stHandle);
	}
};

SessionManager::ConnectAwaiter::ConnectAwaiter(SessionManager* pManagerParam)
	: m_pWait(std::make_shared<ConnectWait>())
{
	pManagerParam->BeginConnect(m_pWait);
}

bool SessionManager::ConnectAwaiter::Suspend(std::coroutine_handle<> stHandleParam, void (*fnScheduleParam)(std::coroutine_handle<>))
{
	// 局部引用保活，发布挂起后工人可以立即恢复并销毁 awaiter。
	auto pWait = m_pWait;
	OI::Thread::MutexGuard stLock(pWait->stMutex);
	// 已完成对象可以再次 await；不能把完成门重新改成挂起状态。
	if (pWait->bDone) return false;
	pWait->stHandle = stHandleParam;
	pWait->fnSchedule = fnScheduleParam;
	return pWait->iGate.exchange(2, std::memory_order_acq_rel) != 1;
}

bool SessionManager::ConnectAwaiter::await_resume() const noexcept { return m_pWait->bResult; }

void SessionManager::FinishConnectWaiters(bool bResultParam)
{
	auto vWaiters = std::move(m_vConnectWaiters);
	m_vConnectWaiters.clear();
	for (const auto& pWeak : vWaiters)
		if (auto pWait = pWeak.lock()) pWait->Finish(bResultParam);
}

void SessionManager::BeginConnect(const std::shared_ptr<ConnectWait>& pWaitParam)
{
	if (OI::Thread::ThreadPool::GetInstance().StopRequested()) { pWaitParam->Finish(false); return; }
	struct ConnectRequest { SessionManager* pManager; std::shared_ptr<ConnectWait> pWait; };
	auto pRequest = std::make_unique<ConnectRequest>(ConnectRequest{this, pWaitParam});
	Poll::GetInstance().CallOnLoop([](void* pArgumentParam) {
		std::unique_ptr<ConnectRequest> pHeld(static_cast<ConnectRequest*>(pArgumentParam));
		auto* pManager = pHeld->pManager;
		if (!pManager->StartConnect()) { pHeld->pWait->Finish(false); return; }
		if (!pManager->bConnectPending) { pHeld->pWait->Finish(true); return; }
		// 不 await 的临时对象不会取消建连；尾部过期占位摊销 O(1) 清理。
		auto& vWaiters = pManager->m_vConnectWaiters;
		while (!vWaiters.empty() && vWaiters.back().expired()) vWaiters.pop_back();
		vWaiters.push_back(pHeld->pWait);
	}, pRequest.get());
	pRequest.release();
}

bool SessionManager::StartConnect()
{
	assert(Poll::GetInstance().InLoopThread());
	if (OI::Thread::ThreadPool::GetInstance().StopRequested()) return false;
	if (bServiceDown) return false;
	if (bConnectPending || HasEndpoint(sIpAddress + ":" + sPort)) return true;
	bConnectPending = true;
	const uint64_t iGeneration = ++iConnectGeneration;
	const bool bStarted = ConnectChannel::StartConnect(this, true);
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
		FinishConnectWaiters(false);
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
	FinishConnectWaiters(false);
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
	FinishConnectWaiters(true);
}

void SessionManager::AbortSession(uint64_t iGenerationParam)
{
	if (iGenerationParam != iConnectGeneration || !bConnectPending) return;
	bConnectPending = false;
	FinishConnectWaiters(false);
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
