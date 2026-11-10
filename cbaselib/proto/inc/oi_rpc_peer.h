#pragma once
#include "oi_header.h"
#include "oi_lock.h"
#include "oi_net_os.h"
#include "oi_rpc_call.h"
#include <list>
#include <memory>
#include <string>
#include <unordered_map>
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

}
}
