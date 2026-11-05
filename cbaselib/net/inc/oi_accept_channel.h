#pragma once
#include "oi_channel.h"
namespace OI
{
namespace Net
{

class SessionManager;
class AcceptChannel : public Channel
{
	SessionManager* m_pSessionManager;

  public:
	AcceptChannel(NetSocket stText, SessionManager* pMgr)
	    : Channel(stText, 0), m_pSessionManager(pMgr)
	{
	}
	virtual void DataIn();
	virtual void DataOut();
	virtual bool TryClose();

  public:
	static bool StartAccept(SessionManager* pMgr, bool bAsync);

  private:
	static bool StartTcpAccept(SessionManager* pMgr, bool bAsync);
};
}
}
