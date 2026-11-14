#pragma once
#include "oi_rpc_session_manager.h"
#include "log_server_session.h"
#include "oi_singleton.h"
class LogServer : public ::OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(LogServer)
	LogServer()
	{
	}

  public:
	virtual ~LogServer();
	int32_t Init() override;
	const std::string GetIDString() override
	{
		return "LogServer";
	}

  private:
	void OnAddSession(::OI::Net::Session* pSession) override;
	void OnDelSession(const ::OI::Net::Session* pSession) override;
	::OI::Net::Session* CloneSession() override
	{
		return new LogServerSession(this);
	} //自定制回话类型
};
