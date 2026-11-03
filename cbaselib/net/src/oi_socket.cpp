#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>


#include <stdlib.h>
#include <string.h>
#include "oi_socket.h"
#include "oi_session_manager.h"
#include "oi_net_helper.h"
#include "oi_log.h"
namespace OI { namespace Net {

NetAddr::NetAddr():m_iProtocolType(AF_UNSPEC)
{
}

bool NetAddr::Init(SessionManager* pMgrParam)
{
	const std::string& sIpLocal = pMgrParam->GetStrIP();
	const std::string& sPortLocal = pMgrParam->GetStrPort();
	m_iProtocolType = AF_INET;
	switch (m_iProtocolType)
	{
		case AF_INET:
		{
			sockaddr_in stAddr;
			memset(&stAddr, 0, sizeof(stAddr));
			stAddr.sin_family = AF_INET;
			char* pEnd = NULL;
			long iPortNumber = strtol(sPortLocal.c_str(), &pEnd, 10);
			if (sPortLocal.empty() || *pEnd != '\0' || iPortNumber < 0 || iPortNumber > 65535)
			{
				LOG_ERROR("NetAddr::Init bad port=%s id=%s", sPortLocal.c_str(), pMgrParam->GetIDString().c_str());
				return false;
			}
			stAddr.sin_port = htons((uint16_t)iPortNumber);
			// 只收点分 IPv4。解析失败不能按 255.255.255.255 去连。
			if (inet_pton(AF_INET, sIpLocal.c_str(), &stAddr.sin_addr) != 1)
			{
				LOG_ERROR("NetAddr::Init bad ip=%s id=%s", sIpLocal.c_str(), pMgrParam->GetIDString().c_str());
				return false;
			}
			m_stAddressData.Clear();
			m_stAddressData.Append(&stAddr, sizeof(stAddr));
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
	return m_iProtocolType != AF_UNSPEC;
}

sockaddr* NetAddr::NetAttrPtr()
{
	return (sockaddr*)m_stAddressData.Begin();
}

socklen_t NetAddr::NetAttrLen()
{
	return (socklen_t)m_stAddressData.Size();
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

NetSocket::NetSocket():m_iSocketDescriptor(INVALID_SOCKET_ID),m_iMask(0)
{
}

bool NetSocket::Init(net_socket_t iSessionIdParam, SessionManager* pMgrParam)
{
	m_iSocketDescriptor = iSessionIdParam;
	m_iMask |= NSM_INIT;
	return m_stAddress.Init(pMgrParam);
}

bool NetSocket::IsBind()
{
	return m_iMask & NSM_BIND;
}

void NetSocket::SetBind()
{
	assert(!IsBind());
	m_iMask |= NSM_BIND;
}

bool NetSocket::IsListen()
{
	return m_iMask & NSM_LISTEN;
}

void NetSocket::SetListen()
{
	assert(!IsListen());
	m_iMask |= NSM_LISTEN;

}

bool NetSocket::IsConnect()
{
	return m_iMask & NSM_CONNECT;
}

void NetSocket::SetConnect()
{
	assert(!IsAccept());
	m_iMask |= NSM_CONNECT;
}

bool NetSocket::IsAccept()
{
	return m_iMask & NSM_ACCEPT;
}


void NetSocket::SetAccept()
{
	assert(!IsConnect());
	m_iMask |= NSM_ACCEPT;
}

bool NetSocket::IsAsync()
{
	return m_iMask & NSM_ASYNC;
}

void NetSocket::SetAsync(bool bAsyncParam)
{
	if (bAsyncParam)
	{
		m_iMask |= NSM_ASYNC;
	}
	else
	{
		m_iMask &= ~NSM_ASYNC;
	}
}

bool NetSocket::CreateSocket(SessionManager* pMgrParam, bool bAsyncParam)
{
	return NetHelper::CreateSocket(*this, pMgrParam, bAsyncParam);
}

bool NetSocket::SocketBind(SessionManager* pMgrParam)
{
	return NetHelper::SocketBind(*this, pMgrParam);
}

bool NetSocket::SocketListen(SessionManager* pMgrParam)
{
	return NetHelper::SocketListen(*this, pMgrParam);
}

bool NetSocket::SocketListenAndBind(SessionManager* pMgrParam)
{
	return NetHelper::SocketListenAndBind(*this, pMgrParam);
}

bool NetSocket::SocketConnect(SessionManager* pMgrParam)
{
	return NetHelper::SocketConnect(*this, pMgrParam);
}

//accept一个套接字,返回-1失败,返回0说明没有可接受的了
int NetSocket::SocketAcceptOne(SessionManager* pMgrParam)
{
	return NetHelper::SocketAcceptOne(*this, pMgrParam);
}

NetAddr NetSocket::GetNetAddr(SessionManager* pMgrParam)
{
	return NetHelper::GetNetAddr(pMgrParam);
}
}}