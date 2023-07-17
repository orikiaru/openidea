#pragma once
#include "bytes.h"
#include <sys/socket.h>
class SessionManager;

class NetAddr
{
	int		_proto_type;
	Bytes	_addr_data;
public:
	NetAddr();
	bool		Init(SessionManager* pMgr);
	bool		IsValidAttr() const;
	sockaddr*	NetAttrPtr();
	socklen_t	NetAttrLen();
};

typedef int net_socket_t;
#define INVALID_SOCKET_ID -1

class NetSocket
{
	NetAddr			_addr;
	net_socket_t	_sid;
	uint32_t		_mask;
public:
	NetSocket();
	virtual ~NetSocket() {}
	bool Init(net_socket_t sid, SessionManager* pMgr);

	bool IsBind();
	void SetBind();

	bool IsListen();
	void SetListen();

	bool IsConnect();
	void SetConnect();

	bool IsAccept();
	void SetAccept();

	bool IsAsync();
	void SetAsync(bool async);
public:
	bool CreateSocket(SessionManager* pMgr, bool async);
	bool SocketBind(SessionManager* pMgr);
	bool SocketListen(SessionManager* pMgr);
	bool SocketListenAndBind(SessionManager* pMgr);
	bool SocketConnect(SessionManager* pMgr);
	//accept一个套接字,返回-1失败,返回0说明没有可接受的了
	int SocketAcceptOne(SessionManager* pMgr);
	NetAddr GetNetAddr(SessionManager* pMgr);
public:
	net_socket_t GetSocket() const	{ return _sid; }
	bool IsValidAttr() const		{ return _addr.IsValidAttr(); }
	NetAddr& GetAddr()				{ return _addr; }
};