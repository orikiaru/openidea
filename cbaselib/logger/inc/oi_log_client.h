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
	const std::string GetIDString() override
	{
		return "LogClient";
	}

  private:
	::OI::Net::Session* CloneSession() override
	{
		return new LogClientSession(this);
	} //自定制回话类型
	void OnClientConnect(::OI::Net::Session* pSession) override;
	void OnClientDisconnect(const ::OI::Net::Session* pSession) override;
};
}
}
