#pragma once
#include "oi_protocol.h"
#include "oi_rpc_call.h"
#include "oi_net.h"
#include "oi_singleton.h"

class IpcServer : public ProtoSessionManager
{
	DECLARE_SINGLETON(IpcServer)
	IpcServer() {}
public:
	virtual ~IpcServer();
	bool Init();
	virtual const std::string GetIDString();
private:
	virtual void OnAddSession(Session* pSession);
	virtual void OnDelSession(const Session* pSession);
};        
