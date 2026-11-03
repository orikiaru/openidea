#pragma once
#include <assert.h>
#include "oi_net_session.h"
namespace OI { namespace Log {

class LogClientSession : public ::OI::Net::NetSession
{
public:
	LogClientSession(::OI::Net::SessionManager* pMgrParam) :NetSession(pMgrParam) {}
	virtual ~LogClientSession() {}
	virtual void OnDataIn();	//raw_data->real_data   decode or decrypt
	virtual void OnDataOut();	//real_data->raw_data 	encode or encrypt
};
}}
