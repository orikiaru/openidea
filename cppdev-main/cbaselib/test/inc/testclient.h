#pragma once
#include "protocol.h"
#include "rpc.h"
#include "net_header.h"

class TestClient : public ProtoClientManager
{
	static TestClient inst;
public:
	TestClient(){}
	virtual ~TestClient();
	bool Init();
	static TestClient& GetInstance()  { return inst; }
	virtual const std::string GetIDString();
private:
	virtual void OnClientConnect(Session* pSession);
	virtual void OnClientDisconnect(const Session* pSession);
	virtual void OnAbortSession();
};
