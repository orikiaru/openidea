#pragma once
#include <string>
#include <deque>
#include <list>
#include <unordered_map>
#include "oi_session_manager.h"
#include "oi_rpc_session.h"
#include "oi_rpc_call.h"
#include "oi_log.h"
namespace OI { namespace Rpc {

void FailRpcCalls(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam);

enum CLIENT_CONNECT_MODE
{
	CLIENT_CONNECT_DIRECT = 0,
	CLIENT_CONNECT_ON_SEND = 1,
};

class ProtoSessionManager : public ::OI::Net::SessionManager
{
	void OnSessionClosed(sid_t iSessionIdParam) override final { FailRpcCalls(this, iSessionIdParam); }
public:
	ProtoSessionManager() {}
	virtual const std::string GetIDString() { return "ProtoSessionManager"; }
	virtual ::OI::Net::Session* CloneSession() { return new ProtoSession(this); }	//自定制回话类型
	virtual void OnAddSession(::OI::Net::Session* pSessionParam)
	{
		LOG_INFO("ProtoSessionManager::OnAddSession sid=%d", pSessionParam->GetSessionID());
	}
	virtual void OnDelSession(const ::OI::Net::Session* pSessionParam)
	{
		LOG_INFO("ProtoSessionManager::OnDelSession sid=%d, error=%d", pSessionParam->GetSessionID(), pSessionParam->GetError());
	}
};

//client只负责连接一个server
//如果需要连接池,需要建立多个client,自行设计吧
class ProtoClientManager : public ProtoSessionManager
{
	sid_t		m_iSessionId;
	bool		m_bConnected;
	bool		m_bConnecting;
	CLIENT_CONNECT_MODE m_iConnectMode;
	struct HeldCall
	{
		uint64_t iCallId;
		::OI::Base::Bytes stData;
	};
	std::deque<::OI::Base::Bytes> m_vPending;
	std::list<HeldCall> m_vHeldCalls;
	std::unordered_map<uint64_t, std::list<HeldCall>::iterator> m_mHeldIndex;
	size_t m_iHeldBytes = 0;
	size_t m_iHeldMaxCount = 65536;
	size_t m_iHeldMaxBytes = 64 * 1024 * 1024;
	size_t		m_iPendingBytes;
	bool		m_bEverConnected;
	::OI::Thread::SpinLock	m_stClientLocker;
public:
	ProtoClientManager():m_iSessionId(INVALID_SID),m_bConnected(false),m_bConnecting(false),m_iConnectMode(CLIENT_CONNECT_DIRECT),m_iPendingBytes(0),m_bEverConnected(false) {}
	void SetConnectMode(CLIENT_CONNECT_MODE iModeParam) { m_iConnectMode = iModeParam; }
	CLIENT_CONNECT_MODE GetConnectMode() const { return m_iConnectMode; }
	virtual void OnClientConnect(::OI::Net::Session* pSessionParam) = 0;
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSessionParam) = 0;

	sid_t ConnectedSid()
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		return m_bConnected ? m_iSessionId : INVALID_SID;
	}
	bool IsConnect()
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		return m_bConnected;
	}
	virtual void OnAddSession(::OI::Net::Session* pSessionParam)
	{
		sid_t iSessionIdLocal = INVALID_SID;
		{
			::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
			m_bConnected = true;
			m_bConnecting = false;
			m_iSessionId = pSessionParam->GetSessionID();
			iSessionIdLocal = m_iSessionId;
			// 与新发送共用客户端锁；先发送连接前缓存，不能让新数据抢占它的额度。
			// 待发总量已受发送额度限制；若连接恰好关闭，保留尚未接纳的部分等重连。
			while (!m_vPending.empty())
			{
				if (!SendBytes(iSessionIdLocal, m_vPending.front())) break;
				m_iPendingBytes -= m_vPending.front().Size();
				m_vPending.pop_front();
			}
			m_bEverConnected = true;
		}
		FlushHeldCalls(iSessionIdLocal);
		OnClientConnect(pSessionParam);
	}
	virtual void OnConnectFailed()
	{
		{
			::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
			m_bConnecting = false;
		}
		if (bServiceDown || sEndpoint.empty())
		{
			FailHeldCalls();
			return;
		}
		Reconnect();
	}
	virtual void OnServiceDown()
	{
		FailHeldCalls();
	}
	virtual int32_t UnavailableCode()
	{
		return IsConnect() ? OI_ERR_FAIL : OI_ERR_CONNECT;
	}
	void SetPendingCallLimits(size_t iCountParam, size_t iBytesParam)
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		m_iHeldMaxCount = iCountParam;
		m_iHeldMaxBytes = iBytesParam;
	}
	bool TryPendCall(uint64_t iCallIdParam, const ::OI::Base::Bytes& stDataParam) override;
	void DropPendingCall(uint64_t iCallIdParam) override;
	void FlushHeldCalls(sid_t iSessionIdParam);
	void FailHeldCalls();
	virtual void OnDelSession(const ::OI::Net::Session* pSessionParam)
	{
		bool bReconnect = false;
		{
			::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
			if (m_iSessionId == pSessionParam->GetSessionID())
			{
				m_iSessionId = INVALID_SID;
				m_bConnected = false;
				bReconnect = !bServiceDown && !sEndpoint.empty() && pSessionParam->GetEndpoint() == sEndpoint;
			}
		}
		OnClientDisconnect(pSessionParam);
		if (bReconnect)
		{
			Reconnect(0);
		}
	}
	bool SendRaw(const ::OI::Base::Bytes& stDataParam);
	// 没连上先存着，连上时按顺序补发。存满 max_pending_bytes 再来的丢掉，返回 false
	bool SendRawOrPend(const ::OI::Base::Bytes& stDataParam, size_t iMaxPendingBytesParam);
	void Reconnect(int iDelayMillisecondsParam = 2000);

};
}}
