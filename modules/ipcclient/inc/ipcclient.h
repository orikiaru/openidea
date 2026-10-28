#pragma once
#include "oi_protocol.h"
#include "oi_rpc_call.h"
#include "oi_net.h"
#include "oi_singleton.h"

class IpcClient : public ::OI::Rpc::ProtoClientManager
{
	DECLARE_SINGLETON(IpcClient)
	IpcClient() {}
public:
	virtual ~IpcClient();
	bool Init();
	virtual const std::string GetIDString();
private:
	virtual void OnClientConnect(::OI::Net::Session* pSession);
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession);
	virtual void OnAbortSession();
};
