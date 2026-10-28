#pragma once

#include "oi_net_session.h"
#include "oi_parser.h"
class ProtoSession : public NetSession
{
	ProtocolParser _parser;
public:
	ProtoSession(SessionManager* pMgr) :NetSession(pMgr) {}
	virtual void Send(const OI::Base::Bytes& bytes);
	virtual void SendProtocol(const Protocol* proto);
	virtual void OnDataIn();	//raw_data->real_data   decode or decrypt
	virtual void OnDataOut();	//real_data->raw_data 	encode or encrypt
};
