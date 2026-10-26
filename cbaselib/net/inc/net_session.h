#pragma once
#include "session.h"
//use for connected net_fd
class NetSession : public Session
{
protected:
	virtual void Close(int err);		//内部调用关闭,必然由IO线程调用,无锁
public:
	NetSession(SessionManager* pMgr) :Session(pMgr) {}
	virtual void ActiveClose();			//主动close,由SessionManager触发或者IO线程触发
	virtual void PassiveClose(int err);	//IO线程被动关闭调用,会回调Mgr->CloseSession
	virtual void OnClose();
};
