#pragma once

#include "oi_net_session.h"
#include "oi_parser.h"
namespace OI
{
namespace Rpc
{
class ProtoSession : public ::OI::Net::NetSession
{
	struct PartialFrameTimer;
	PartialFrameTimer* m_pIncompleteTimer = nullptr;
	void ArmIncompleteTimer();
	void CancelIncompleteTimer();

  public:
	ProtoSession(::OI::Net::SessionManager* pMgrParam)
	    : NetSession(pMgrParam)
	{
	}
	virtual void OnDataIn();  //raw_data->real_data   decode or decrypt
	virtual void OnDataOut(); //real_data->raw_data 	encode or encrypt
	void OnClose() override;
};
}
}
