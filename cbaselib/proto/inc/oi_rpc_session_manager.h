#pragma once
#include <string>
#include <deque>
#include <list>
#include <unordered_map>
#include "oi_session_manager.h"
#include "oi_rpc_session.h"
#include "oi_rpc_call.h"
#include "oi_log.h"
#include "oi_duration.h"
namespace OI
{
namespace Rpc
{

template <typename T>
struct RpcProtoId;

void FailRpcCalls(::OI::Net::SessionManager* pManager, sid_t iSessionId);

enum CLIENT_CONNECT_MODE
{
	CLIENT_CONNECT_DIRECT = 0,
	CLIENT_CONNECT_ON_SEND = 1,
};

class ProtoSessionManager : public ::OI::Net::SessionManager
{
	void OnSessionClosed(sid_t iSessionId) override final
	{
		FailRpcCalls(this, iSessionId);
	}

  public:
	ProtoSessionManager()
	{
	}
	bool Init() override
	{
		return InitRpcAdmission() && ::OI::Net::SessionManager::Init();
	}
	virtual const std::string GetIDString()
	{
		return "ProtoSessionManager";
	}
	virtual ::OI::Net::Session* CloneSession()
	{
		return new ProtoSession(this);
	} //自定制回话类型
	virtual void OnAddSession(::OI::Net::Session* pSession)
	{
		LOG_INFO("ProtoSessionManager::OnAddSession sid=%d", pSession->GetSessionID());
	}
	virtual void OnDelSession(const ::OI::Net::Session* pSession)
	{
		LOG_INFO("ProtoSessionManager::OnDelSession sid=%d, error=%d", pSession->GetSessionID(), pSession->GetError());
	}
};

//client只负责连接一个server
//如果需要连接池,需要建立多个client,自行设计吧
class ProtoClientManager : public ProtoSessionManager
{
	RpcSender m_stSender;
	sid_t m_iSessionId;
	bool m_bConnected;
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
	size_t m_iPendingBytes;
	bool m_bEverConnected;
	::OI::Thread::SpinLock m_stClientLocker;
	bool HoldCallLocked(uint64_t iCallId, const ::OI::Base::Bytes& stData);

  protected:
	virtual bool AutoReconnect() const
	{
		return true;
	}

  public:
	ProtoClientManager()
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
	RpcSender& Sender()
	{
		return m_stSender;
	}
	void SetConnectMode(CLIENT_CONNECT_MODE iMode)
	{
		m_iConnectMode = iMode;
	}
	CLIENT_CONNECT_MODE GetConnectMode() const
	{
		return m_iConnectMode;
	}
	virtual void OnClientConnect(::OI::Net::Session* pSession) = 0;
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession) = 0;

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
	virtual void OnAddSession(::OI::Net::Session* pSession)
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
				if (!SendBytes(iSessionIdLocal, m_vPending.front()))
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
	virtual void OnConnectFailed()
	{
		if (!AutoReconnect() || bServiceDown || sEndpoint.empty())
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
	void SetPendingCallLimits(size_t iCount, size_t iBytes)
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		m_iHeldMaxCount = iCount;
		m_iHeldMaxBytes = iBytes;
	}
	std::pair<size_t, size_t> PendingCallLimits()
	{
		::OI::Thread::SpinLockGuard stLock(m_stClientLocker);
		return {m_iHeldMaxCount, m_iHeldMaxBytes};
	}
	bool TryPendCall(uint64_t iCallId, const ::OI::Base::Bytes& stData) override;
	// 提交时在客户端锁内决定排队或绑定当前 sid，随后发送，避免构造等待对象时截取旧 sid。
	virtual void SubmitCall(uint64_t iCallId, const ::OI::Base::Bytes& stData);
	void DropPendingCall(uint64_t iCallId) override;
	void FlushHeldCalls(sid_t iSessionId);
	void FailHeldCalls();
	virtual void OnDelSession(const ::OI::Net::Session* pSession)
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
	// 输出引用版自动配对响应类型，返回 TaskResult<int32_t>；输出须活到任务结束。
	// Call(req,res) 使用当前连接；CallAny 按名字选路；Call(zone,req,res) 指定分区。
	template <typename Req>
	auto Call(Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	template <typename Req>
	auto CallAny(Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	template <typename Req>
	auto Call(int32_t iZoneId, Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	// 不带分区号：在当前登记里选一台。本管理器已经连着这个名字时继续用这条连接。
	template <typename Res, typename Req>
	auto CallAny(Req stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	// 指定分区号，只打这一台。全局服不接受分区号。
	template <typename Res, typename Req>
	auto Call(int32_t iZoneId, Req stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	// 按发送这一刻的名单，向每个分片都发一份。
	template <typename Res, typename Req>
	auto CallAll(const Req& stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	// 只打名单里的这些分区，结果顺序和分区号一致。没登记的那一格记连接失败。
	template <typename Res, typename Req>
	auto CallMany(const std::vector<int32_t>& vZoneIds, const Req& stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	bool SendRaw(const ::OI::Base::Bytes& stData);
	// 没连上先存着，连上时按顺序补发。存满 max_pending_bytes 再来的丢掉，返回 false
	bool SendRawOrPend(const ::OI::Base::Bytes& stData, size_t iMaxPendingBytes);
	void Reconnect(int iDelayMilliseconds = 2000);
};
}
}
