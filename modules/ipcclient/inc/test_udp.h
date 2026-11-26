#pragma once
#include "oi_net.h"
#include "oi_rpc_session_manager.h"
#include "oi_rpcdata_udpnotereq.h"
#include "oi_singleton.h"
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// 客户端的 UDP 口。端口填 0 让系统分配，对端用 OpenPeer 打开。
class IpcUdp : public ::OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(IpcUdp)
	std::atomic<bool> m_bAddPeerAddressOk{false};
	std::atomic<int32_t> m_iAddSessionCount{0};
	IpcUdp()
	{
	}

  public:
	const std::string GetIDString() override;
	int32_t AddSessionCount() const
	{
		return m_iAddSessionCount.load();
	}
	bool AddPeerAddressOk() const
	{
		return m_bAddPeerAddressOk.load();
	}

  private:
	void OnAddSession(::OI::Net::Session* pSession) override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;
};

// UDP 会话、RPC、单向消息、大包、坏包和空闲回收。关掉就不跑。
class TestUdp
{
	DECLARE_SINGLETON(TestUdp)
	TestUdp();
	bool m_bEnabled;
	std::string m_sServerIp;
	std::string m_sServerPort;
	int32_t m_iTotal;
	int32_t m_iNotes;
	std::atomic<int32_t> m_iNext;
	std::atomic<int32_t> m_iRpcBad;
	std::atomic<sid_t> m_iClosedSessionId;
	std::atomic<int32_t> m_iNoteReceived;
	std::atomic<int32_t> m_iNoteDuplicate;
	std::atomic<int32_t> m_iPeerNoteMask{0};
	std::atomic<int32_t> m_iPeerNoteDuplicate{0};
	std::unique_ptr<std::atomic<uint8_t>[]> m_pNoteSeen;

	::OI::Rpc::RpcTask HandleNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest);
	OI::Coro::Task Run();
	OI::Coro::Task Pump(sid_t iSessionId);

  public:
	int32_t Load();
	bool Enabled() const
	{
		return m_bEnabled;
	}
	void Start();
	void OnClose(sid_t iSessionId);
};
