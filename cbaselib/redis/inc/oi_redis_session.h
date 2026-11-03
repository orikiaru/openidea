#pragma once
#include "oi_net_session.h"
namespace OI { namespace Redis {

class RedisClientSession : public ::OI::Net::NetSession
{
public:
	RedisClientSession(::OI::Net::SessionManager* pManagerParam) : ::OI::Net::NetSession(pManagerParam) {}
	virtual void OnDataIn();
	virtual void OnDataOut();
};
}}
