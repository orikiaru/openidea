#pragma once
#include "oi_net_session.h"

class RedisServer;

class RedisServerSession : public ::OI::Net::NetSession
{
	RedisServer* m_pOwner;
	int32_t m_iLink;
	bool m_bReadPaused = false;
	size_t ProcessInput(size_t iBudgetParam);
public:
	RedisServerSession(RedisServer* pOwnerParam);
	virtual ~RedisServerSession();
	void Bind(int32_t iLinkParam) { m_iLink = iLinkParam; }
	bool ResumeRead();
	virtual void OnDataIn();
	virtual void OnDataOut();
};
