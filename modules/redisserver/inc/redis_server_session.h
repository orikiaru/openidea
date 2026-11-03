#pragma once
#include "oi_net_session.h"

class RedisServer;

class RedisServerSession : public ::OI::Net::NetSession
{
	RedisServer* m_pOwner;
	int32_t m_iLink;
	bool m_bReadPaused = false;
public:
	RedisServerSession(RedisServer* pOwnerParam);
	virtual ~RedisServerSession();
	void Bind(int32_t iLinkParam) { m_iLink = iLinkParam; }
	void ResumeRead();
	virtual void OnDataIn();
	virtual void OnDataOut();
};
