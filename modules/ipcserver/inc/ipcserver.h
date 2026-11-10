#pragma once
#include "oi_rpc_call.h"
#include "oi_rpc_coro.h"
#include "oi_net.h"
#include "oi_rpcdata_peerprobereq.h"
#include "oi_rpcdata_peerproberes.h"
#include "oi_singleton.h"

class IpcServer : public ::OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(IpcServer)
	IpcServer()
	{
	}
	int32_t m_iServiceId = 0;

  public:
	virtual ~IpcServer();
	bool Init();
	// [HttpProbe] 配了 host 才发一次 GET，打印结果。不等它，监听照常。
	void StartHttpProbe();
	virtual const std::string GetIDString();
	// 回本进程的 [Service] id，多连接回归用来分辨打到了哪台。
	::OI::Rpc::RpcTask HandlePeerProbe(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::PeerProbeReq& stRequest, ::OI::RpcData::PeerProbeRes& stResponse);

  private:
	virtual void OnAddSession(::OI::Net::Session* pSession);
	virtual void OnDelSession(const ::OI::Net::Session* pSession);
};
