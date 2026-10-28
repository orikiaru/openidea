#pragma once
#include "protocol.h"
#include "rpc.h"
#include "net_header.h"
#include "singleton.h"

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
