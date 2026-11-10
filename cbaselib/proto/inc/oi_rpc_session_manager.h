#pragma once
#include <string>
#include <atomic>
#include <deque>
#include <list>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>
#include "oi_session_manager.h"
#include "oi_rpc_session.h"
#include "oi_rpc_call.h"
#include "oi_rpc_peer.h"
#include "oi_registry.h"
#include "oi_log.h"
#include "oi_duration.h"
namespace OI
{
namespace Rpc
{

template <typename T>
struct RpcProtoId;
struct ServiceRoute;

// 定义在 oi_rpc_coro.h。会话不在时由 SendBytesResult 返回 OI_ERR_CONNECT，编码失败是 OI_ERR_ENCODE。
template <typename Msg>
int32_t RpcNotify(::OI::Net::SessionManager& stManager, sid_t iSessionId, const Msg& stMessage);

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
	// 向这条会话发一条单向消息。sid 从 ConnectedSid、OpenPeer 或 RpcContext 来。
	template <typename Msg>
	int32_t Notify(sid_t iSessionId, const Msg& stMessage)
	{
		return RpcNotify(*this, iSessionId, stMessage);
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
	// 固定地址连接建好后纳入本管理器的会话表，只在 poll 线程调用。
	sid_t AdoptPeer(net_socket_t iSocketLocal, const std::shared_ptr<PeerConnection>& pConnection);
	// 固定地址连接连上、连接前暂存的请求补发之后回调，在 poll 线程。默认什么都不做。
	virtual void OnPeerConnect(int32_t, sid_t)
	{
	}
};

// client 只连一个 server。同一服务名下每台实例各连一条，用 ProtoMultiClientManager。
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

// 同一服务名下每台实例各一条常驻连接，断开或连不上自动重连。
// 配了 <段名>.service：跟着注册表的整份名单，实例号取登记的 [Service] id；注册里查不到时先连配置地址。
// 没配：只有一条连到 <段名>.ip/port 的连接，实例号是 0。
class ProtoMultiClientManager : public ProtoSessionManager
{
	::OI::Thread::Mutex m_stPeerLocker;
	std::map<int32_t, std::shared_ptr<PeerConnection>> m_mPeers;
	std::vector<::OI::Base::ServiceInstance> m_vSeed;
	bool m_bListed = false;
	bool m_bStarted = false;
	std::atomic<uint32_t> m_iCursor{0};
	void ApplyPeers(std::map<int32_t, std::pair<std::string, std::string>> mTargets);
	void RetirePeer(const std::shared_ptr<PeerConnection>& pPeer);

  protected:
	void BindService(const std::string& sService, std::string& sIpLocal, std::string& sPortLocal) override;
	void WatchService(const std::string& sService) override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;
	// 连不上之后隔多久再连。
	virtual int RetryMilliseconds() const
	{
		return 2000;
	}
	// 某台断开，在 poll 线程。这台的在途调用已经回 OI_ERR_CONNECT。
	virtual void OnPeerDisconnect(int32_t, sid_t)
	{
	}

  public:
	bool Init() override;
	// 对当前名单里的每台发起建连。之后新登记的实例自动连。
	void Start();
	// 单地址建连不适用于这里。
	ConnectAwaiter StartClient() = delete;
	// 当前名单，按实例号升序。
	std::vector<int32_t> InstanceIds();
	// 没这台或没连上返回 INVALID_SID。
	sid_t ConnectedSid(int32_t iInstanceId);
	// 名单里没有这台返回空。
	std::shared_ptr<RpcSender> PeerSender(int32_t iInstanceId);
	// 在已连上的实例里轮询；一台都没连上时在名单里轮询。名单为空返回空。
	std::shared_ptr<RpcSender> AnySender();
	// 按实例号升序取当前名单的发送目标，vIds 与 stRoute.vClients 一一对应。
	void AllSenders(std::vector<int32_t>& vIds, ServiceRoute& stRoute);
	// 注册表下发这个名字的整份名单。按实例号比对：多出的建连，消失的退役，地址变了的先退役再新建。
	void ApplyServiceList(const std::vector<::OI::Base::ServiceInstance>& vList) override;

	// 给这台发单向消息。没连上返回 OI_ERR_CONNECT。
	template <typename Msg>
	int32_t NotifyPeer(int32_t iInstanceId, const Msg& stMessage);
	// 指定一台。名单里没有返回 OI_ERR_CONNECT；在名单里还没连上就先暂存，连上补发。返回 TaskResult<int32_t>。
	template <typename Req>
	auto Call(int32_t iInstanceId, Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	// 任选一台，规则同 AnySender。
	template <typename Req>
	auto CallAny(Req stRequest, typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
	// 按发送这一刻的名单给每台各发一份，先全部发出再等。结果按实例号升序，带实例号。
	template <typename Res, typename Req>
	auto CallAll(const Req& stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180));
};
}
}
