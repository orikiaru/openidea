#pragma once
#include "protocol.h"
#include "rpc.h"
#include "net_header.h"

class IpcServer : public ProtoSessionManager
{
	static IpcServer inst;
public:
	IpcServer(){}
	virtual ~IpcServer();
	bool Init();
	static IpcServer& GetInstance() { return inst; }
	virtual const std::string GetIDString();
private:
	virtual void OnAddSession(Session* pSession);
	virtual void OnDelSession(const Session* pSession);
};        
