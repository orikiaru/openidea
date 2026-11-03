#pragma once
#include "oi_rpc_session_manager.h"
#include "oi_log_client_session.h"
#include "oi_singleton.h"
namespace OI { namespace Log {
class LogClient : public ::OI::Rpc::ProtoClientManager
{
	DECLARE_SINGLETON(LogClient)
	LogClient() {}
public:
	virtual ~LogClient();
	bool Init();
	bool SendLog(int iLevelParam, const char* pTextParam, size_t iTextLengthParam);
	virtual const std::string GetIDString() { return "LogClient"; }
private:
	virtual ::OI::Net::Session* CloneSession() { return new LogClientSession(this); }	//自定制回话类型
	virtual void OnClientConnect(::OI::Net::Session* pSessionParam);
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSessionParam);
};
}}
