#pragma once
#include "oi_net_session.h"
namespace OI { namespace Redis {

class RedisClientSession : public ::OI::Net::NetSession
{
public:
	RedisClientSession(::OI::Net::SessionManager* mgr) : ::OI::Net::NetSession(mgr) {}
	virtual void Send(const ::OI::Base::Bytes& bytes);
	virtual void OnDataIn();
	virtual void OnDataOut();
};
}}
