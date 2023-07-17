#pragma once

#include <deque>
#include <list>

#include "net_session.h"
#include "parser.h"
class ProtoSession : public NetSession
{
	ProtocolParser _parser;

	typedef std::deque<Bytes> MSG_QUEUE;
	Mutex		_queue_locker;
	MSG_QUEUE	_msg_queue;
public:
	ProtoSession(SessionManager* pMgr) :NetSession(pMgr) {}
	virtual void SendProtocol(const Protocol* proto);
	virtual void OnDataIn();	//raw_data->real_data   decode or decrypt
	virtual void OnDataOut();	//real_data->raw_data 	encode or encrypt
};
