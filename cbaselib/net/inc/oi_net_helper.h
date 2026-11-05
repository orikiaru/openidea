#pragma once

#include "oi_socket.h"
namespace OI
{
namespace Net
{

class SessionManager;

class NetHelper
{
  public:
	static bool CreateSocket(NetSocket& stSocket, SessionManager* pMgr, bool bAsync);
	static bool SocketBind(NetSocket& stSessionId, SessionManager* pMgr);
	static bool SocketListen(NetSocket& stSessionId, SessionManager* pMgr);
	static bool SocketListenAndBind(NetSocket& stSessionId, SessionManager* pMgr);
	static bool SocketConnect(NetSocket& stSessionId, SessionManager* pMgr);
	//accept一个套接字,返回-1失败,返回0说明没有可接受的了
	static int SocketAcceptOne(NetSocket& stSessionId, SessionManager* pMgr);
	static NetAddr GetNetAddr(SessionManager* pMgr);
};
}
}