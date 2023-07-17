#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>


#include "net_socket.h"
#include "session_manager.h"
#include "net_helper.h"

NetAddr::NetAddr():_proto_type(AF_UNSPEC)
{
}

bool NetAddr::Init(SessionManager* pMgr)
{
	const std::string& ip = pMgr->GetStrIP();
	const std::string& port = pMgr->GetStrPort();
	_proto_type = AF_INET;
	switch (_proto_type)
	{
		case AF_INET:
		{
			sockaddr_in addr;
			addr.sin_family = AF_INET;
			addr.sin_port = htons(atoi(port.c_str()));
			addr.sin_addr.s_addr = inet_addr(ip.c_str());
			_addr_data.Append(&addr, sizeof(addr));
		}break;
		//TODO AF_INET6
		default:
		{
			assert(false);
			return false;
		}break;
	}
	return true;
}


bool NetAddr::IsValidAttr() const
{
	return _proto_type != AF_UNSPEC;
}

sockaddr* NetAddr::NetAttrPtr()
{
	return (sockaddr*)_addr_data.Begin();
}

socklen_t NetAddr::NetAttrLen()
{
	return (socklen_t)_addr_data.Size();
}

enum NET_SOCKET_MASK
{
	NSM_INIT	= 0x0001,
	NSM_ASYNC	= 0x0002,
	NSM_BIND	= 0x0004,
	NSM_LISTEN	= 0x0008,
	NSM_CONNECT = 0x0010,
	NSM_ACCEPT	= 0x0020,
};

NetSocket::NetSocket():_sid(INVALID_SOCKET_ID),_mask(0)
{
}

bool NetSocket::Init(net_socket_t sid, SessionManager* pMgr)
{
	_sid = sid;
	_mask |= NSM_INIT;
	return _addr.Init(pMgr);
}

bool NetSocket::IsBind()
{
	return _mask & NSM_BIND;
}

void NetSocket::SetBind()
{
	assert(!IsBind());
	_mask |= NSM_BIND;
}

bool NetSocket::IsListen()
{
	return _mask & NSM_LISTEN;
}

void NetSocket::SetListen()
{
	assert(!IsListen());
	_mask |= NSM_LISTEN;

}

bool NetSocket::IsConnect()
{
	return _mask & NSM_CONNECT;
}

void NetSocket::SetConnect()
{
	assert(!IsAccept());
	_mask |= NSM_CONNECT;
}

bool NetSocket::IsAccept()
{
	return _mask & NSM_ACCEPT;
}


void NetSocket::SetAccept()
{
	assert(!IsConnect());
	_mask |= NSM_ACCEPT;
}

bool NetSocket::IsAsync()
{
	return _mask & NSM_ASYNC;
}

void NetSocket::SetAsync(bool async)
{
	if (async)
	{
		_mask |= NSM_ASYNC;
	}
	else
	{
		_mask &= ~NSM_ASYNC;
	}
}

bool NetSocket::CreateSocket(SessionManager* pMgr, bool async)
{
	return NetHelper::CreateSocket(*this, pMgr, async);
}

bool NetSocket::SocketBind(SessionManager* pMgr)
{
	return NetHelper::SocketBind(*this, pMgr);
}

bool NetSocket::SocketListen(SessionManager* pMgr)
{
	return NetHelper::SocketListen(*this, pMgr);
}

bool NetSocket::SocketListenAndBind(SessionManager* pMgr)
{
	return NetHelper::SocketListenAndBind(*this, pMgr);
}

bool NetSocket::SocketConnect(SessionManager* pMgr)
{
	return NetHelper::SocketConnect(*this, pMgr);
}

//accept一个套接字,返回-1失败,返回0说明没有可接受的了
int NetSocket::SocketAcceptOne(SessionManager* pMgr)
{
	return NetHelper::SocketAcceptOne(*this, pMgr);
}

NetAddr NetSocket::GetNetAddr(SessionManager* pMgr)
{
	return NetHelper::GetNetAddr(pMgr);
}