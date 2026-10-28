#pragma once
#include "proto_session_manager.h"
#include "log_client_session.h"
#include "singleton.h"
class LogClient : public ProtoClientManager
{
	DECLARE_SINGLETON(LogClient)
	LogClient() {}
public:
	virtual ~LogClient();
	bool Init();
	bool SendLog(int level, const char* text, size_t text_len);
	virtual const std::string GetIDString() { return "LogClient"; }
private:
	virtual Session* CloneSession() { return new LogClientSession(this); }	//自定制回话类型
	virtual void OnClientConnect(Session* pSession);
	virtual void OnClientDisconnect(const Session* pSession);
	virtual void OnAbortSession();
};
