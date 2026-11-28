#pragma once
#include "oi_lock.h"
#include "oi_net.h"
#include "oi_rpc_session_manager.h"
#include "oi_rpcdata_peernotereq.h"
#include "oi_singleton.h"
#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <vector>

// 多连接客户端。配置段名由构造参数给，段里配 service 走注册表，不配就是写死地址的一台，实例号 0。
class MultiClient : public ::OI::Rpc::ProtoMultiClientManager
{
	std::string m_sName;
	::OI::Thread::Mutex m_stCountLocker;
	std::map<int32_t, int32_t> m_mConnects;
	std::map<int32_t, int32_t> m_mDisconnects;
	std::atomic<int32_t> m_iNotes{0};
	std::atomic<int32_t> m_iNoteBad{0};

  protected:
	void OnPeerDisconnect(int32_t iInstanceId, sid_t iSessionId) override;

  public:
	explicit MultiClient(std::string sName);
	const std::string GetIDString() override;
	void OnPeerConnect(int32_t iInstanceId, sid_t iSessionId) override;
	::OI::Rpc::RpcTask HandleNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::PeerNoteReq& stRequest);
	int32_t Connects(int32_t iInstanceId);
	int32_t Disconnects(int32_t iInstanceId);
	int32_t Notes() const
	{
		return m_iNotes.load();
	}
	int32_t NoteBad() const
	{
		return m_iNoteBad.load();
	}
};

// 写死地址和注册表两种多连接：建连回调、按实例号调用、任选、广播、服务端反推、写死地址换台、一台下线再上线。关掉就不跑。
class TestMulti
{
	DECLARE_SINGLETON(TestMulti)
	TestMulti();
	bool m_bEnabled;
	bool m_bPool;
	MultiClient m_stFixed;
	MultiClient m_stPeers;

	static OI::Coro::TaskResult<int32_t> CheckHostedService(int32_t iSequence);
	static OI::Coro::TaskResult<int32_t> CheckRouteLimits(bool bConfigured);
	OI::Coro::Task Run();
	// 本地名单或 registry 拉到的名单上，CallAny 要打到每一台，CallMany 里不存在的实例号回 CONNECT。
	OI::Coro::Task Pool();
	// 写死地址换到 2 号再换回来。返回 0 表示都对上。
	OI::Coro::TaskResult<int32_t> SwapFixed();
	// 成功回服务端的 [Service] id，失败回错误码，标记对不上回 OI_ERR_FAIL。
	static OI::Coro::TaskResult<int32_t> ProbeOnce(MultiClient& stClient, int32_t iInstanceId, int32_t iTag, int32_t iNotify);
	static OI::Coro::TaskResult<bool> WaitFor(std::function<bool()> fnReady, int64_t iMilliseconds);
	static std::string JoinIds(const std::vector<int32_t>& vIds);
	// 问这台 peerserver 收到了几条该标记的 PeerNote。失败回错误码。
	OI::Coro::TaskResult<int32_t> QueryPeerNotes(int32_t iInstanceId, int32_t iTag);
	// 等到这台的计数等于期望，或到点。
	OI::Coro::TaskResult<int32_t> WaitPeerNotes(int32_t iInstanceId, int32_t iTag, int32_t iExpected, int64_t iMilliseconds);
	// NotifyAll 每台一条，NotifyMany 跳过名单外的实例。计数写回调用方，返回值是检查失败数。
	OI::Coro::TaskResult<int> CheckNotifyBatch(int32_t& iAllStarted, int32_t& iAllGot, int32_t& iManyStarted, int32_t& iManyGot);
	// 关掉一台后立刻 NotifyMany 带上它，重连后必须送到。返回 1 表示送到了。
	OI::Coro::TaskResult<int32_t> CheckNotifyReconnect();

  public:
	int32_t Load();
	void Start();
};
