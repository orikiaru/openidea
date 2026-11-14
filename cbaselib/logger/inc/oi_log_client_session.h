#pragma once
#include <assert.h>
#include "oi_net_session.h"
namespace OI
{
namespace Log
{

class LogClientSession : public ::OI::Net::NetSession
{
  public:
	LogClientSession(::OI::Net::SessionManager* pMgr)
	    : NetSession(pMgr)
	{
	}
	virtual ~LogClientSession()
	{
	}
	void OnDataIn() override;  //raw_data->real_data   decode or decrypt
	void OnDataOut() override; //real_data->raw_data 	encode or encrypt
};
}
}
