#pragma once
#include "proto_session_manager.h"
#include "log_server_session.h"
#include "singleton.h"
class LogServer : public ProtoSessionManager
{
	DECLARE_SINGLETON(LogServer)
	LogServer() {}
public:
	virtual ~LogServer();
	bool Init();
	virtual const std::string GetIDString() { return "LogServer"; }
private:
	virtual void OnAddSession(Session* pSession);
	virtual void OnDelSession(const Session* pSession);
	virtual Session* CloneSession() { return new LogServerSession(this); }	//自定制回话类型
};
