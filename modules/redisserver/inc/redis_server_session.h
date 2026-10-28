#pragma once
#include "oi_net_session.h"

class RedisServer;

class RedisServerSession : public ::OI::Net::NetSession
{
	RedisServer* _owner;
	int32_t _link;
public:
	RedisServerSession(RedisServer* owner);
	virtual ~RedisServerSession();
	void Bind(int32_t link) { _link = link; }
	virtual void Send(const ::OI::Base::Bytes& bytes);
	virtual void SendProtocol(const ::OI::Rpc::Protocol* proto);
	virtual void OnDataIn();
	virtual void OnDataOut();
};
