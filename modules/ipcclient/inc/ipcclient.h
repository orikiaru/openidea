#pragma once
#include "oi_rpc_call.h"
#include "oi_net.h"
#include "oi_singleton.h"
#include <atomic>

class IpcClient : public ::OI::Rpc::ProtoClientManager
{
	DECLARE_SINGLETON(IpcClient)
	IpcClient()
	{
	}
	std::atomic<bool> m_bTestsStarted{false};

  public:
	virtual ~IpcClient();
	bool Init();
	virtual const std::string GetIDString();

  private:
	virtual void OnClientConnect(::OI::Net::Session* pSession);
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession);
};
