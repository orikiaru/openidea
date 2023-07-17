#pragma once
#include "channel.h"
class SessionManager;
class ConnectChannel : public Channel
{
	bool _close;
	SessionManager* _p_session_mgr;
public:
	ConnectChannel(NetSocket s, SessionManager* pMgr) :Channel(s, 0), _close(false), _p_session_mgr(pMgr) {}
	virtual void DataIn();
	virtual void DataOut();
	virtual bool TryClose();
private:
	void Close()	{ _close = true; }
	bool IsClosed() { return _close;	 }
	bool CheckTcpConnect();
	bool CheckSslConnect();
public:
	static bool StartConnect(SessionManager* pMgr, bool async);
private:
	static bool StartTcpConnect(SessionManager* pMgr, bool async);
	static bool StartSslConnect(SessionManager* pMgr, bool async);
};
