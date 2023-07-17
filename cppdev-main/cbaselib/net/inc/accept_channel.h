#pragma once
#include "channel.h"

class SessionManager;
class AcceptChannel : public Channel
{
	SessionManager* _p_session_mgr;
public:
	AcceptChannel(NetSocket s, SessionManager* pMgr) :Channel(s, 0), _p_session_mgr(pMgr) {}
	virtual void DataIn();
	virtual void DataOut();
	virtual bool TryClose();
public:
	static bool StartAccept(SessionManager* pMgr, bool async);
private:
	static bool StartTcpAccept(SessionManager* pMgr, bool async);
	static bool StartSslAccept(SessionManager* pMgr, bool async);
};
