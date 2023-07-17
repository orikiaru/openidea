#pragma once
#include "protocol.h"
#include "rpc.h"
#include "net_header.h"

class TestServer : public ProtoSessionManager
{
	static TestServer inst;
public:
	TestServer(){}
	virtual ~TestServer();
	bool Init();
	static TestServer& GetInstance() { return inst; }
	virtual const std::string GetIDString();
private:
	virtual void OnAddSession(Session* pSession);
	virtual void OnDelSession(const Session* pSession);
};        
