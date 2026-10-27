#pragma once
#include "net_session.h"

class RedisClientSession : public NetSession
{
public:
	RedisClientSession(SessionManager* mgr) : NetSession(mgr) {}
	virtual void Send(const Bytes& bytes);
	virtual void SendProtocol(const Protocol* proto);
	virtual void OnDataIn();
	virtual void OnDataOut();
};
