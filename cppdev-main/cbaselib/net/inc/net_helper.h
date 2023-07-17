#pragma once

#include "net_socket.h"

class SessionManager;


class NetHelper
{
public:
	static bool CreateSocket(NetSocket& socket, SessionManager* pMgr, bool async);
	static bool SocketBind(NetSocket& sid, SessionManager* pMgr);
	static bool SocketListen(NetSocket& sid, SessionManager* pMgr);
	static bool SocketListenAndBind(NetSocket& sid, SessionManager* pMgr);
	static bool SocketConnect(NetSocket& sid, SessionManager* pMgr);
	//accept一个套接字,返回-1失败,返回0说明没有可接受的了
	static int SocketAcceptOne(NetSocket& sid, SessionManager* pMgr);
	static NetAddr GetNetAddr(SessionManager* pMgr);
};