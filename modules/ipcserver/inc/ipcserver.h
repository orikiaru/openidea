#pragma once
#include "oi_protocol.h"
#include "oi_rpc_call.h"
#include "oi_net.h"
#include "oi_singleton.h"

class IpcServer : public ::OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(IpcServer)
	IpcServer() {}
public:
	virtual ~IpcServer();
	bool Init();
	// [HttpProbe] 配了 host 才发一次 GET，打印结果。不等它，监听照常。
	void StartHttpProbe();
	virtual const std::string GetIDString();
private:
	virtual void OnAddSession(::OI::Net::Session* pSession);
	virtual void OnDelSession(const ::OI::Net::Session* pSession);
};        
