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
	size_t m_iFrameBytes;
	uint64_t m_iFrameTimeoutMilliseconds;
	// UDP 会话：一包里只放整帧，坏包和残包整包丢掉，不关会话，也不等后续字节。
	bool m_bDatagram;

  public:
	ProtoSession(::OI::Net::SessionManager* pMgr);
	void SetFrameLimits(size_t iBytes, uint64_t iTimeoutMilliseconds)
	{
		m_iFrameBytes = iBytes;
		m_iFrameTimeoutMilliseconds = iTimeoutMilliseconds;
	}
	virtual void OnDataIn();  //raw_data->real_data   decode or decrypt
	virtual void OnDataOut(); //real_data->raw_data 	encode or encrypt
	void OnClose() override;
};
}
}
