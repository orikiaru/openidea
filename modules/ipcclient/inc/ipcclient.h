#pragma once
#include "protocol.h"
#include "rpc.h"
#include "net_header.h"
#include "singleton.h"

class IpcClient : public ProtoClientManager
{
	DECLARE_SINGLETON(IpcClient)
	IpcClient() {}
public:
	virtual ~IpcClient();
	bool Init();
	virtual const std::string GetIDString();
private:
	virtual void OnClientConnect(Session* pSession);
	virtual void OnClientDisconnect(const Session* pSession);
	virtual void OnAbortSession();
};
