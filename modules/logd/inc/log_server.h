#pragma once
#include "oi_rpc_session_manager.h"
#include "log_server_session.h"
#include "oi_singleton.h"
class LogServer : public ::OI::Rpc::ProtoSessionManager
{
	DECLARE_SINGLETON(LogServer)
	LogServer() {}
public:
	virtual ~LogServer();
	bool Init();
	virtual const std::string GetIDString() { return "LogServer"; }
private:
	virtual void OnAddSession(::OI::Net::Session* pSession);
	virtual void OnDelSession(const ::OI::Net::Session* pSession);
	virtual ::OI::Net::Session* CloneSession() { return new LogServerSession(this); }	//自定制回话类型
};
