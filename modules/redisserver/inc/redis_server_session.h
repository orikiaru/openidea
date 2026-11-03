#pragma once
#include "oi_net_session.h"

class RedisServer;

class RedisServerSession : public ::OI::Net::NetSession
{
	RedisServer* m_pOwner;
	int32_t m_iLink;
public:
	RedisServerSession(RedisServer* pOwnerParam);
	virtual ~RedisServerSession();
	void Bind(int32_t iLinkParam) { m_iLink = iLinkParam; }
	virtual void OnDataIn();
	virtual void OnDataOut();
};
