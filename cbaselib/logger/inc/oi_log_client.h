#pragma once
#include "oi_rpc_session_manager.h"
#include "oi_log_client_session.h"
#include "oi_singleton.h"
namespace OI
{
namespace Log
{
class LogClient : public ::OI::Rpc::ProtoClientManager
{
	DECLARE_SINGLETON(LogClient)
	LogClient()
	{
	}

  public:
	virtual ~LogClient();
	int32_t Init() override;
	int32_t SendLog(int iLevel, const char* pText, size_t iTextLength);
	virtual const std::string GetIDString()
	{
		return "LogClient";
	}

  private:
	virtual ::OI::Net::Session* CloneSession()
	{
		return new LogClientSession(this);
	} //自定制回话类型
	virtual void OnClientConnect(::OI::Net::Session* pSession);
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession);
};
}
}
