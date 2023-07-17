#pragma once
#include "net_session.h"
#include "log_parser.h"
#include "log_protocol.h"

class LogClientSession : public NetSession
{
	static LogProtocolParser _parser;
public:
	LogClientSession(SessionManager* pMgr) :NetSession(pMgr) {}
	virtual ~LogClientSession() {}
	virtual void SendProtocol(const LogProtocol* proto);
	virtual void SendProtocol(const Protocol* proto)
	{
		SendProtocol(dynamic_cast<const LogProtocol*>(proto));
	}
	virtual void OnDataIn();	//raw_data->real_data   decode or decrypt
	virtual void OnDataOut();	//real_data->raw_data 	encode or encrypt
};
