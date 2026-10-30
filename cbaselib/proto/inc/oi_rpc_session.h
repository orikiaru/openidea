#pragma once

#include "oi_net_session.h"
#include "oi_parser.h"
namespace OI { namespace Rpc {
class ProtoSession : public ::OI::Net::NetSession
{
public:
	ProtoSession(::OI::Net::SessionManager* pMgr) :NetSession(pMgr) {}
	virtual void Send(const ::OI::Base::Bytes& bytes);
	virtual void OnDataIn();	//raw_data->real_data   decode or decrypt
	virtual void OnDataOut();	//real_data->raw_data 	encode or encrypt
};
}}
