#pragma once
#include "oi_header.h"
#include "oi_lock.h"
#include "oi_net_os.h"
#include "oi_rpc_call.h"
#include "oi_registry.h"
#include <atomic>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
namespace OI
{
namespace Net
{
class NetAddr;
}
namespace Rpc
{
class ProtoSessionManager;
class PeerConnector;
struct ServiceRoute;

// 一条固定地址连接的收发上限、帧上限和连接前暂存额度，取自发起方管理器。
struct PeerPolicy
{
	size_t iSendBytes = 64 * 1024 * 1024;
	size_t iReceiveBytes = 64 * 1024 * 1024 + 8;
	size_t iFrameBytes = 64 * 1024 * 1024;
	uint64_t iIncompleteMilliseconds = 10000;
	size_t iHeldCount = 65536;
	size_t iHeldBytes = 64 * 1024 * 1024;

	explicit PeerPolicy(::OI::Net::SessionManager* pManager);
	bool operator<(const PeerPolicy& stOther) const;
};

// 一条连接上等连接的暂存请求，诊断用。
struct PeerHeldInfo
{
	size_t iCount = 0;
	size_t iBytes = 0;
};

// 到一个固定地址的连接状态。会话进 pOwner 的会话表，回调和 RpcContext::stManager 都是 pOwner。
// 常驻连接断开或连不上时按 iRetryMilliseconds 重连，直到 Retire；非常驻连接只在有请求时建连。
struct PeerConnection
{
	struct Held
	{
		uint64_t iCallId;
		::OI::Base::Bytes stData;
	};
	std::weak_ptr<PeerConnection> pSelf;
	RpcSender stSender;
	ProtoSessionManager* const pOwner;
	const int32_t iInstanceId;
	const std::string sIp;
	const std::string sPort;
	const PeerPolicy stPolicy;
	const bool bPersistent;
	const int iRetryMilliseconds;
	::OI::Thread::Mutex stMutex;
	std::weak_ptr<RpcSender> pLease;
	sid_t iSessionId = INVALID_SID;
	bool bConnecting = false;
	bool bPosted = false;
	bool bRetired = false;
	PeerConnector* pConnector = nullptr; // 只在 poll 线程操作。
	std::list<Held> vHeld;
	std::unordered_map<uint64_t, std::list<Held>::iterator> mHeld;
	size_t iHeldBytes = 0;

	static std::shared_ptr<PeerConnection> Create(ProtoSessionManager* pOwner, int32_t iInstanceId, std::string sIp, std::string sPort,
	    PeerPolicy stPolicy, bool bPersistent, int iRetryMilliseconds = 2000);
	PeerConnection(ProtoSessionManager* pOwner, int32_t iInstanceId, std::string sIp, std::string sPort, PeerPolicy stPolicy, bool bPersistent,
	    int iRetryMilliseconds);
	PeerConnection(const PeerConnection&) = delete;
	PeerConnection& operator=(const PeerConnection&) = delete;

	// 业务租约独立于会话持有的连接状态，路由池据此判断空闲。
	std::shared_ptr<RpcSender> AcquireLease();
	bool HasLease();
	// 发送目标和连接状态同生命周期，调用期间持有它即可。
	std::shared_ptr<RpcSender> SharedSender();
	sid_t SessionId();
	PeerHeldInfo HeldInfo();
	// 没连上也没在连时切到 poll 线程发起建连。
	void Ensure();
	// 只在 poll 线程调用：取消建连、关会话，暂存的请求回 OI_ERR_CONNECT，之后不再重连。
	void Retire();
	void Submit(uint64_t iCallId, const ::OI::Base::Bytes& stData);
	void Drop(uint64_t iCallId);

	void Begin();
	void Connect(::OI::Net::NetAddr stAddr);
	static void OnResolved(void* pArgument, bool bOk, ::OI::Net::NetAddr stAddr);
	void Connected(net_socket_t iSocketLocal);
	void Finish(sid_t iSessionIdLocal);
	void Disconnected(sid_t iSessionIdLocal);
	void ScheduleRetry();
};

// 一个服务名下的常驻连接，按实例号排。不是会话管理器，会话进 pOwner 的会话表。
class ServiceGroup
{
	struct Member
	{
		std::shared_ptr<PeerConnection> pConnection;
		std::string sZone;
	};
	ProtoSessionManager* const m_pOwner;
	::OI::Thread::Mutex m_stMutex;
	std::map<int32_t, Member> m_mMembers;
	bool m_bStarted;
	std::atomic<uint32_t> m_iCursor{0};
	std::vector<std::shared_ptr<PeerConnection>> Connections();
	static void Retire(const std::shared_ptr<PeerConnection>& pConnection);

  public:
	// bStarted 为假时新连接只登记不建连，等 Start。
	ServiceGroup(ProtoSessionManager* pOwner, bool bStarted);
	ServiceGroup(const ServiceGroup&) = delete;
	ServiceGroup& operator=(const ServiceGroup&) = delete;
	// 按实例号比对：多出的建连，消失的退役，地址或分区变了的先退役再新建。iRetryMilliseconds 只用于新建的连接。
	void Apply(const std::vector<::OI::Base::ServiceInstance>& vList, int iRetryMilliseconds);
	void Start();
	// 全部退役，在途调用回 OI_ERR_CONNECT。
	void RetireAll();
	// 没这台返回空。
	std::shared_ptr<RpcSender> Peer(int32_t iInstanceId);
	// 在已连上的里轮询；一台都没连上时在全部里轮询，调用先暂存。名单为空返回空。
	std::shared_ptr<RpcSender> Any();
	// 这个分区里实例号最小的一台，没有返回空。
	std::shared_ptr<RpcSender> Zone(const std::string& sZone);
	// 按实例号升序，vIds 与 stRoute.vClients 一一对应。
	void All(std::vector<int32_t>& vIds, ServiceRoute& stRoute);
	std::vector<int32_t> Ids();
	// 没这台或没连上返回 INVALID_SID。
	sid_t SessionId(int32_t iInstanceId);
	// 没这台返回全 0。
	PeerHeldInfo HeldInfo(int32_t iInstanceId);
	// 只在已连上的里轮询。一台都没连上返回 INVALID_SID。
	sid_t ConnectedAny();
	size_t Size();
};

}
}
