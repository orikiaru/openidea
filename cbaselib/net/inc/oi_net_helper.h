#pragma once

#include "oi_socket.h"
namespace OI
{
namespace Net
{

class SessionManager;

// 套接字系统调用的集中实现。NetSocket 的同名方法转到这里。
class NetHelper
{
  public:
	// 按管理器类型新建套接字。bAsync 为真时设成非阻塞。
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