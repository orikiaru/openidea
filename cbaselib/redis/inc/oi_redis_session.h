#pragma once
#include "oi_net_session.h"
namespace OI
{
namespace Redis
{

class RedisClientSession : public ::OI::Net::NetSession
{
  public:
	RedisClientSession(::OI::Net::SessionManager* pManager)
	    : ::OI::Net::NetSession(pManager)
	{
	}
	void OnDataIn() override;
	void OnDataOut() override;
};
}
}
