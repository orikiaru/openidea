#include "ipcudp.h"
#include "oi_rpcdata_udpnoteres.h"
#include "oi_socket.h"
#include "rpc_ids.h"
#include <stdio.h>

const std::string IpcUdp::GetIDString()
{
	return "IpcUdp";
}

::OI::Rpc::RpcTask IpcUdp::HandleEcho(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpEchoReq& stRequest, ::OI::RpcData::UdpEchoRes& stResponse)
{
	stResponse.iSeq = stRequest.iSeq;
	if (stRequest.iSeq == ::OI::RpcData::UDP_PROBE_REJECT)
	{
		co_return ::OI::RpcData::UDP_ERR_REJECTED;
	}
	if (stRequest.iSeq == ::OI::RpcData::UDP_PROBE_PEER_ADDR)
	{
		::OI::Net::NetAddr stAddr;
		if (!GetPeerAddr(stContext.iSessionId, stAddr))
		{
			co_return OI_ERR_NOT_FOUND;
		}
		stResponse.sData = stAddr.Endpoint();
		co_return OI_OK;
	}
	if (stRequest.iSeq == ::OI::RpcData::UDP_PROBE_SESSIONS)
	{
		stResponse.sData = std::to_string(m_iSessions.load());
		co_return OI_OK;
	}
	stResponse.sData = stRequest.sData;
	co_return OI_OK;
}

::OI::Rpc::RpcTask IpcUdp::HandleNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::UdpNoteReq& stRequest)
{
	co_return stContext.Notify(stRequest);
}

bool IpcUdp::Init()
{
	OI_RPC_BIND_SELF(UdpEcho, IpcUdp, HandleEcho);
	OI_NOTIFY_BIND_SELF(UdpNote, IpcUdp, HandleNote);
	return ::OI::Rpc::ProtoSessionManager::Init();
}

void IpcUdp::OnAddSession(::OI::Net::Session* pSession)
{
	(void)pSession;
	m_iSessions.fetch_add(1);
}

void IpcUdp::OnDelSession(const ::OI::Net::Session* pSession)
{
	(void)pSession;
}
