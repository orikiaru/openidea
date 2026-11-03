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
	static bool CreateSocket(NetSocket& stSocketParam, SessionManager* pMgrParam, bool bAsyncParam);
	static bool SocketBind(NetSocket& stSessionIdParam, SessionManager* pMgrParam);
	static bool SocketListen(NetSocket& stSessionIdParam, SessionManager* pMgrParam);
	static bool SocketListenAndBind(NetSocket& stSessionIdParam, SessionManager* pMgrParam);
	static bool SocketConnect(NetSocket& stSessionIdParam, SessionManager* pMgrParam);
	//accept一个套接字,返回-1失败,返回0说明没有可接受的了
	static int SocketAcceptOne(NetSocket& stSessionIdParam, SessionManager* pMgrParam);
	static NetAddr GetNetAddr(SessionManager* pMgrParam);
};
}
}