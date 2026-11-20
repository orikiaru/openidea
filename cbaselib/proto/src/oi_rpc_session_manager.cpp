#include "oi_rpc_session_manager.h"
#include "oi_base.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
namespace OI
{
namespace Rpc
{

void ProtoSessionManager::BindRpcHandler(proto_type_t iId, RpcHandleFn fnHandle)
{
	bool bInserted = false;
	{
		::OI::Thread::MutexGuard stLock(m_stHandlerLocker);
		bInserted = m_mHandlers.emplace(iId, std::move(fnHandle)).second;
	}
	if (!bInserted)
	{
		LOG_ERROR("rpc handler already bound manager=%s proto=%u", GetIDString().c_str(), (unsigned)iId);
		fprintf(stderr, "rpc handler already bound manager=%s proto=%u\n", GetIDString().c_str(), (unsigned)iId);
		fflush(stderr);
		throw std::logic_error("rpc handler already bound manager=" + GetIDString() + " proto=" + std::to_string((unsigned)iId));
	}
}

const RpcHandleFn* ProtoSessionManager::FindRpcHandler(proto_type_t iId)
{
	::OI::Thread::MutexGuard stLock(m_stHandlerLocker);
	auto it = m_mHandlers.find(iId);
	if (it == m_mHandlers.end())
	{
		return nullptr;
	}
	return &it->second;
}

int32_t ProtoSessionManager::Init()
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoSessionManager::Init");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	const int32_t iAdmissionStatus = RpcCallManager::GetInstance().InitAdmission();
	if (iAdmissionStatus != OI_OK)
	{
		return iAdmissionStatus;
	}
	return ::OI::Net::SessionManager::Init();
}

void ProtoSessionManager::OnAddSession(::OI::Net::Session* pSession)
{
	LOG_INFO("ProtoSessionManager::OnAddSession sid=%d", pSession->GetSessionID());
}

void ProtoSessionManager::OnDelSession(const ::OI::Net::Session* pSession)
{
	LOG_INFO("ProtoSessionManager::OnDelSession sid=%d, error=%d", pSession->GetSessionID(), pSession->GetError());
}

ProtoClientManager::ProtoClientManager()
    : m_iSessionId(INVALID_SID), m_bConnected(false), m_iConnectMode(CLIENT_CONNECT_DIRECT), m_iPendingBytes(0), m_bEverConnected(false)
{
	m_stSender = {this, this, [](void* pContext, uint64_t iCallId, const ::OI::Base::Bytes& stData)
	{
		static_cast<ProtoClientManager*>(pContext)->SubmitCall(iCallId, stData);
	}, [](void* pContext, uint64_t iCallId)
	{
		static_cast<ProtoClientManager*>(pContext)->DropPendingCall(iCallId);
	}};
}

sid_t ProtoClientManager::ConnectedSid()
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoClientManager::ConnectedSid()") != OI_OK)
	{
		return INVALID_SID;
	}
	::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
	return m_bConnected ? m_iSessionId : INVALID_SID;
}

bool ProtoClientManager::IsConnect()
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoClientManager::IsConnect()") != OI_OK)
	{
		return false;
	}
	::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
	return m_bConnected;
}

void ProtoClientManager::OnAddSession(::OI::Net::Session* pSession)
{
	sid_t iSessionIdLocal = INVALID_SID;
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		m_bConnected = true;
		m_iSessionId = pSession->GetSessionID();
		iSessionIdLocal = m_iSessionId;
		// 与新发送共用客户端锁；先发送连接前缓存，不能让新数据抢占它的额度。
		// 待发总量已受发送额度限制；若连接恰好关闭，保留尚未接纳的部分等重连。
		while (!m_vPending.empty())
		{
			if (SendBytes(iSessionIdLocal, m_vPending.front()) != OI_OK)
			{
				break;
			}
			m_iPendingBytes -= m_vPending.front().Size();
			m_vPending.pop_front();
		}
		m_bEverConnected = true;
	}
	FlushHeldCalls(iSessionIdLocal);
	OnClientConnect(pSession);
}

void ProtoClientManager::OnConnectFailed()
{
	if (!AutoReconnect() || bServiceDown || sEndpoint.empty())
	{
		FailHeldCalls();
		return;
	}
	Reconnect();
}

void ProtoClientManager::SetPendingCallLimits(size_t iCount, size_t iBytes)
{
	::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
	m_iHeldMaxCount = iCount;
	m_iHeldMaxBytes = iBytes;
}

std::pair<size_t, size_t> ProtoClientManager::PendingCallLimits()
{
	::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
	return {m_iHeldMaxCount, m_iHeldMaxBytes};
}

void ProtoClientManager::OnDelSession(const ::OI::Net::Session* pSession)
{
	bool bReconnect = false;
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		if (m_iSessionId == pSession->GetSessionID())
		{
			m_iSessionId = INVALID_SID;
			m_bConnected = false;
			bReconnect = AutoReconnect() && !bServiceDown && !sEndpoint.empty() && pSession->GetEndpoint() == sEndpoint;
		}
	}
	OnClientDisconnect(pSession);
	if (bReconnect)
	{
		Reconnect(0);
	}
}

int32_t ProtoClientManager::SendRaw(const ::OI::Base::Bytes& stData)
{
	return SendRawResult(stData);
}

int32_t ProtoClientManager::SendRawResult(const ::OI::Base::Bytes& stData)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoClientManager::SendRaw");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	sid_t iSessionIdLocal = INVALID_SID;
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		if (m_bConnected && m_iSessionId != INVALID_SID)
		{
			iSessionIdLocal = m_iSessionId;
		}
	}
	if (iSessionIdLocal == INVALID_SID)
	{
		return OI_ERR_CONNECT;
	}
	return SendBytesResult(iSessionIdLocal, stData);
}

int32_t ProtoClientManager::SendRawOrPend(const ::OI::Base::Bytes& stData, size_t iMaxPendingBytes)
{
	return SendRawOrPendResult(stData, iMaxPendingBytes);
}

int32_t ProtoClientManager::SendRawOrPendResult(const ::OI::Base::Bytes& stData, size_t iMaxPendingBytes)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ProtoClientManager::SendRawOrPend");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	sid_t iSessionIdLocal = INVALID_SID;
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		if (m_bConnected && m_iSessionId != INVALID_SID)
		{
			iSessionIdLocal = m_iSessionId;
		}
		else
		{
			iMaxPendingBytes = (std::min)(iMaxPendingBytes, iMaxSendQueueBytes);
			if (m_iPendingBytes > iMaxPendingBytes || stData.Size() > iMaxPendingBytes - m_iPendingBytes)
			{
				return OI_ERR_BUSY;
			}
			m_vPending.push_back(stData);
			m_iPendingBytes += stData.Size();
			return OI_OK;
		}
	}
	return SendBytesResult(iSessionIdLocal, stData);
}

bool ProtoClientManager::HoldCallLocked(uint64_t iCallId, const ::OI::Base::Bytes& stData)
{
	if (m_vHeldCalls.size() >= m_iHeldMaxCount || m_iHeldBytes > m_iHeldMaxBytes || stData.Size() > m_iHeldMaxBytes - m_iHeldBytes)
	{
		return false;
	}
	// 先标记再入队；超时回调会在客户端锁释放后删除。已经超时则不再留副本。
	if (!RpcCallManager::GetInstance().MarkAwaitingConnect(iCallId))
	{
		return true;
	}
	m_vHeldCalls.push_back(HeldCall{iCallId, stData});
	try
	{
		m_mHeldIndex.emplace(iCallId, std::prev(m_vHeldCalls.end()));
	}
	catch (...)
	{
		m_vHeldCalls.pop_back();
		throw;
	}
	m_iHeldBytes += stData.Size();
	return true;
}

bool ProtoClientManager::TryPendCall(uint64_t iCallId, const ::OI::Base::Bytes& stData)
{
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		if (m_bEverConnected || (m_bConnected && m_iSessionId != INVALID_SID))
		{
			return false;
		}
		if (HoldCallLocked(iCallId, stData))
		{
			return true;
		}
	}
	// 不持客户端锁唤醒，回调允许再次发起请求。
	RpcCallManager::GetInstance().Wake(iCallId, OI_ERR_BUSY, nullptr);
	return true;
}

void ProtoClientManager::SubmitCall(uint64_t iCallId, const ::OI::Base::Bytes& stData)
{
	sid_t iSessionIdLocal = INVALID_SID;
	int32_t iFailure = OI_ERR_CONNECT;
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		if (m_bConnected && m_iSessionId != INVALID_SID)
		{
			iSessionIdLocal = m_iSessionId;
			// 来源匹配必须与发送目标一致；已经超时的调用不再发出。
			if (!RpcCallManager::GetInstance().ClaimSend(iCallId, iSessionIdLocal))
			{
				return;
			}
		}
		else if (!m_bEverConnected)
		{
			if (HoldCallLocked(iCallId, stData))
			{
				return;
			}
			iFailure = OI_ERR_BUSY;
		}
	}
	if (iSessionIdLocal != INVALID_SID)
	{
		iFailure = SendFrame(iSessionIdLocal, stData);
		if (iFailure == OI_OK)
		{
			return;
		}
	}
	// 完成可能反过来删除待发项，不能持客户端锁唤醒。
	RpcCallManager::GetInstance().Wake(iCallId, iFailure, nullptr);
}

void ProtoClientManager::DropPendingCall(uint64_t iCallId)
{
	::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
	auto stIterator = m_mHeldIndex.find(iCallId);
	if (stIterator == m_mHeldIndex.end())
	{
		return;
	}
	m_iHeldBytes -= stIterator->second->stData.Size();
	m_vHeldCalls.erase(stIterator->second);
	m_mHeldIndex.erase(stIterator);
}

void ProtoClientManager::FlushHeldCalls(sid_t iSessionId)
{
	std::list<HeldCall> vHeld;
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		vHeld.swap(m_vHeldCalls);
		m_mHeldIndex.clear();
		m_iHeldBytes = 0;
	}
	for (const auto& stCall : vHeld)
	{
		if (!RpcCallManager::GetInstance().ClaimSend(stCall.iCallId, iSessionId))
		{
			continue;
		}
		const int32_t iRet = SendFrame(iSessionId, stCall.stData);
		if (iRet != OI_OK)
		{
			RpcCallManager::GetInstance().Wake(stCall.iCallId, iRet, NULL);
		}
	}
}

void ProtoClientManager::FailHeldCalls()
{
	std::list<HeldCall> vHeld;
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		vHeld.swap(m_vHeldCalls);
		m_mHeldIndex.clear();
		m_iHeldBytes = 0;
		m_vPending.clear();
		m_iPendingBytes = 0;
	}
	for (const auto& stCall : vHeld)
	{
		RpcCallManager::GetInstance().Wake(stCall.iCallId, OI_ERR_CONNECT, NULL);
	}
}

int32_t ProtoClientManager::Reconnect(int iMillisecond)
{
	return ::OI::Timer::BaseTimerManager::GetInstance().AddTimer([this]()
	{
		StartClient();
	}, iMillisecond > 0 ? iMillisecond : 1);
}
}
}
