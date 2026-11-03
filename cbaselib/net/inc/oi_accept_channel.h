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
	AcceptChannel(NetSocket stTextParam, SessionManager* pMgrParam)
	    : Channel(stTextParam, 0), m_pSessionManager(pMgrParam)
	{
	}
	virtual void DataIn();
	virtual void DataOut();
	virtual bool TryClose();

  public:
	static bool StartAccept(SessionManager* pMgrParam, bool bAsyncParam);

  private:
	static bool StartTcpAccept(SessionManager* pMgrParam, bool bAsyncParam);
};
}
}
