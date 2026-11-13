#include "oi_rpc_session_manager.h"
#include "oi_base.h"
#include "oi_rpc_coro.h"
#include <algorithm>
namespace OI
{
namespace Rpc
{

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
	if (!MarkRpcAwaitingConnect(iCallId))
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
	WakeRpcCall(iCallId, OI_ERR_BUSY, nullptr);
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
			if (!ClaimRpcSend(iCallId, iSessionIdLocal))
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
		iFailure = SendBytesResult(iSessionIdLocal, stData);
		if (iFailure == OI_OK)
		{
			return;
		}
	}
	// 完成可能反过来删除待发项，不能持客户端锁唤醒。
	WakeRpcCall(iCallId, iFailure, nullptr);
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
		if (!ClaimRpcSend(stCall.iCallId, iSessionId))
		{
			continue;
		}
		const int32_t iRet = SendBytesResult(iSessionId, stCall.stData);
		if (iRet != OI_OK)
		{
			WakeRpcCall(stCall.iCallId, iRet, NULL);
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
		WakeRpcCall(stCall.iCallId, OI_ERR_CONNECT, NULL);
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
