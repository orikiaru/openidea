#pragma once
#include "rpc_ids.h"
#include "oi_lock.h"
#include "oi_rpc_call.h"
#include "oi_rpc_session_manager.h"
#include "oi_net.h"
#include "oi_rpcdata_peernotereq.h"
#include "oi_rpcdata_peerprobereq.h"
#include "oi_rpcdata_peerproberes.h"
#include "oi_singleton.h"
#include <map>

class IpcServer : public ::OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(IpcServer)
	IpcServer()
	{
	}
	int32_t m_iServiceId = 0;
	// 按 PeerNote.iTag 计数。通知处理和探测查询都会碰到。
	::OI::Thread::Mutex m_stNoteLocker;
	std::map<int32_t, int32_t> m_mNoteCount;
	void BindHold();
	::OI::Rpc::RpcTask HandleAllEcho(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::AllEchoReq& stRequest, ::OI::RpcData::AllEchoRes& stResponse);
	::OI::Rpc::RpcTask HandleEmpty(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::EmptyReq& stRequest, ::OI::RpcData::EmptyRes& stResponse);
	::OI::Rpc::RpcTask HandlePeerNote(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::PeerNoteReq& stRequest);
	// New protocol handler declarations are inserted here.
	int32_t NoteCount(int32_t iTag);

  public:
	void RegisterProtocols();
	virtual ~IpcServer();
	int32_t Init() override;
	// [HttpProbe] 配了 host 才发一次 GET，打印结果。不等它，监听照常。
	void StartHttpProbe();
	const std::string GetIDString() override;
	// 回本进程的 [Service] id，多连接回归用来分辨打到了哪台。iNoteCount 是这个标记已经收到的 PeerNote。
	::OI::Rpc::RpcTask HandlePeerProbe(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::PeerProbeReq& stRequest, ::OI::RpcData::PeerProbeRes& stResponse);

  private:
	void OnAddSession(::OI::Net::Session* pSession) override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;
};
