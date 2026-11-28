#pragma once
#include "oi_net.h"
#include "oi_rpc_session_manager.h"
#include "oi_rpcdata_udpechoreq.h"
#include "oi_rpcdata_udpechores.h"
#include "oi_rpcdata_udpnotereq.h"
#include "oi_singleton.h"
#include <atomic>

// UDP 回归的服务端。一个对端地址一条会话，RPC 和单向消息都走生成的 rpcdata。
class IpcUdp : public ::OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(IpcUdp)
	IpcUdp()
	{
	}
	// New protocol handler declarations are inserted here.
	std::atomic<int32_t> m_iSessions{0};

	::OI::Rpc::RpcTask HandleUdpEcho(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpEchoReq& stRequest, ::OI::RpcData::UdpEchoRes& stResponse);
	::OI::Rpc::RpcTask HandleUdpNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest);

  public:
	void RegisterProtocols();
	int32_t Init() override;
	const std::string GetIDString() override;

  private:
	void OnAddSession(::OI::Net::Session* pSession) override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;
};
