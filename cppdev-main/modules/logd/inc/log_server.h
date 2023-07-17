#pragma once
#include "proto_session_manager.h"
#include "log_server_session.h"
class LogServer : public ProtoSessionManager
{
	static LogServer inst;
public:
	LogServer(){}
	virtual ~LogServer();
	bool Init();
	static LogServer& GetInstance() { return inst; }
	virtual const std::string GetIDString() { return "LogServer"; }
private:
	virtual void OnAddSession(Session* pSession);
	virtual void OnDelSession(const Session* pSession);
	virtual Session* CloneSession() { return new LogServerSession(this); }	//自定制回话类型
};
