#pragma once
#include "proto_session_manager.h"
#include "log_client_session.h"
class LogClient : public ProtoClientManager
{
	static LogClient inst;
public:
	LogClient(){}
	virtual ~LogClient();
	bool Init();
	static LogClient& GetInstance()	{ return inst; }
	virtual const std::string GetIDString() { return "LogClient"; }
private:
	virtual Session* CloneSession() { return new LogClientSession(this); }	//自定制回话类型
	virtual void OnClientConnect(Session* pSession);
	virtual void OnClientDisconnect(const Session* pSession);
	virtual void OnAbortSession();
};
