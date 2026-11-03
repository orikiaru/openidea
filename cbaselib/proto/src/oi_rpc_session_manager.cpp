#include "oi_rpc_session_manager.h"
#include "oi_base.h"
#include "oi_rpc_coro.h"
#include <algorithm>
namespace OI { namespace Rpc {

bool ProtoClientManager::SendRaw(const ::OI::Base::Bytes& stDataParam)
{
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
		return false;
	}
	return SendBytes(iSessionIdLocal, stDataParam);
}

bool ProtoClientManager::SendRawOrPend(const ::OI::Base::Bytes& stDataParam, size_t iMaxPendingBytesParam)
{
	sid_t iSessionIdLocal = INVALID_SID;
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		if (m_bConnected && m_iSessionId != INVALID_SID)
		{
			iSessionIdLocal = m_iSessionId;
		}
		else
		{
			iMaxPendingBytesParam = (std::min)(iMaxPendingBytesParam, iMaxSendQueueBytes);
			if (m_iPendingBytes > iMaxPendingBytesParam || stDataParam.Size() > iMaxPendingBytesParam - m_iPendingBytes)
			{
				return false;
			}
			m_vPending.push_back(stDataParam);
			m_iPendingBytes += stDataParam.Size();
			return true;
		}
	}
	return SendBytes(iSessionIdLocal, stDataParam);
}

bool ProtoClientManager::TryPendCall(uint64_t iCallIdParam, const ::OI::Base::Bytes& stDataParam)
{
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		if (m_bEverConnected || (m_bConnected && m_iSessionId != INVALID_SID)) return false;
		if (m_vHeldCalls.size() < m_iHeldMaxCount && m_iHeldBytes <= m_iHeldMaxBytes
			&& stDataParam.Size() <= m_iHeldMaxBytes - m_iHeldBytes)
		{
			// 先标记再入队；超时回调会在本锁释放后删除。已经超时则不再留副本。
			if (!MarkRpcAwaitingConnect(iCallIdParam)) return true;
			m_vHeldCalls.push_back(HeldCall{iCallIdParam, stDataParam});
			try { m_mHeldIndex.emplace(iCallIdParam, std::prev(m_vHeldCalls.end())); }
			catch (...)
			{
				m_vHeldCalls.pop_back();
				throw;
			}
			m_iHeldBytes += stDataParam.Size();
			return true;
		}
	}
	// 不持客户端锁唤醒，回调允许再次发起请求。
	WakeRpcCall(iCallIdParam, OI_ERR_BUSY, nullptr);
	return true;
}

void ProtoClientManager::DropPendingCall(uint64_t iCallIdParam)
{
	::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
	auto stIterator = m_mHeldIndex.find(iCallIdParam);
	if (stIterator == m_mHeldIndex.end()) return;
	m_iHeldBytes -= stIterator->second->stData.Size();
	m_vHeldCalls.erase(stIterator->second);
	m_mHeldIndex.erase(stIterator);
}

void ProtoClientManager::FlushHeldCalls(sid_t iSessionIdParam)
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
		if (!ClaimRpcSend(stCall.iCallId, iSessionIdParam))
		{
			continue;
		}
		if (!SendBytes(iSessionIdParam, stCall.stData))
		{
			WakeRpcCall(stCall.iCallId, OI_ERR_FAIL, NULL);
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

void ProtoClientManager::Reconnect(int iMillisecondParam)
{	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([this]() { StartClient(); }, iMillisecondParam > 0 ? iMillisecondParam : 1);
}
}}
