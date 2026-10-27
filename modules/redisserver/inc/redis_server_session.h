#pragma once
#include "net_session.h"

struct redisReader;
class RedisServer;

class RedisServerSession : public NetSession
{
	RedisServer* _owner;
	redisReader* _reader;
	int _link;
public:
	RedisServerSession(RedisServer* owner);
	virtual ~RedisServerSession();
	void Bind(int link) { _link = link; }
	virtual void Send(const Bytes& bytes);
	virtual void SendProtocol(const Protocol* proto);
	virtual void OnDataIn();
	virtual void OnDataOut();
};
