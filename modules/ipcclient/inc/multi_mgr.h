#pragma once
#include "oi_lock.h"
#include "oi_net.h"
#include "oi_rpc_coro.h"
#include "oi_rpcdata_peernotereq.h"
#include "oi_singleton.h"
#include <atomic>
#include <map>
#include <string>

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

// 写死地址和注册表两种多连接：建连回调、按实例号调用、任选、广播、服务端反推、一台下线再上线。关掉就不跑。
class MultiMgr
{
	DECLARE_SINGLETON(MultiMgr)
	MultiMgr();
	bool m_bEnabled;
	MultiClient m_stFixed;
	MultiClient m_stPeers;

	OI::Coro::Task Run();

  public:
	bool Load();
	void Start();
};
