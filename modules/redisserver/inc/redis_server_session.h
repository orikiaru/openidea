#pragma once
#include "net_session.h"

class RedisServer;

class RedisServerSession : public NetSession
{
	RedisServer* _owner;
	int _link;
public:
	RedisServerSession(RedisServer* owner);
	virtual ~RedisServerSession();
	void Bind(int link) { _link = link; }
	virtual void Send(const OI::Base::Bytes& bytes);
	virtual void SendProtocol(const Protocol* proto);
	virtual void OnDataIn();
	virtual void OnDataOut();
};
