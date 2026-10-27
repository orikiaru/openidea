#pragma once
#include <assert.h>
#include "net_session.h"

class LogClientSession : public NetSession
{
public:
	LogClientSession(SessionManager* pMgr) :NetSession(pMgr) {}
	virtual ~LogClientSession() {}
	virtual void Send(const Bytes& bytes);
	virtual void SendProtocol(const Protocol* proto)
	{
		(void)proto;
		assert(false && "log client sends raw frames");
	}
	virtual void OnDataIn();	//raw_data->real_data   decode or decrypt
	virtual void OnDataOut();	//real_data->raw_data 	encode or encrypt
};
