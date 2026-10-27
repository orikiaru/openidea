#pragma once
#include "protocol.h"
#include "rpc.h"
#include "net_header.h"

class IpcClient : public ProtoClientManager
{
	static IpcClient inst;
public:
	IpcClient(){}
	virtual ~IpcClient();
	bool Init();
	static IpcClient& GetInstance()  { return inst; }
	virtual const std::string GetIDString();
private:
	virtual void OnClientConnect(Session* pSession);
	virtual void OnClientDisconnect(const Session* pSession);
	virtual void OnAbortSession();
};
