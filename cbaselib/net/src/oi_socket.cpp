#include <sys/types.h> /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <netdb.h>
#endif
#include "oi_socket.h"
#include "oi_session_manager.h"
#include "oi_net_helper.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "uv.h"
namespace OI
{
namespace Net
{

Ipv4Addr::Ipv4Addr()
{
	memset(&m_stAddr, 0, sizeof(m_stAddr));
}

bool Ipv4Addr::Init(const std::string& sAddress, uint16_t iPort)
{
	sockaddr_in stAddr;
	memset(&stAddr, 0, sizeof(stAddr));
	if (inet_pton(AF_INET, sAddress.c_str(), &stAddr.sin_addr) != 1)
	{
		return false;
	}
	stAddr.sin_family = AF_INET;
	stAddr.sin_port = htons(iPort);
	m_stAddr = stAddr;
	return true;
}

bool Ipv4Addr::Init(const sockaddr* pAddr, socklen_t iLength)
{
	if (pAddr == NULL || pAddr->sa_family != AF_INET || iLength < (socklen_t)sizeof(sockaddr_in))
	{
		return false;
	}
	memcpy(&m_stAddr, pAddr, sizeof(m_stAddr));
	return true;
}

std::string Ipv4Addr::Host() const
{
	char sText[INET_ADDRSTRLEN];
	memset(sText, 0, sizeof(sText));
	if (inet_ntop(AF_INET, &m_stAddr.sin_addr, sText, sizeof(sText)) == NULL)
	{
		return std::string();
	}
	return sText;
}

uint16_t Ipv4Addr::Port() const
{
	return ntohs(m_stAddr.sin_port);
}

std::string Ipv4Addr::Endpoint() const
{
	const std::string sHost = Host();
	if (sHost.empty())
	{
		return std::string();
	}
	return sHost + ":" + std::to_string(ntohs(m_stAddr.sin_port));
}

bool Ipv4Addr::IsUnspecified() const
{
	return m_stAddr.sin_addr.s_addr == htonl(INADDR_ANY);
}

sockaddr* Ipv4Addr::NetAttrPtr()
{
	return (sockaddr*)&m_stAddr;
}

socklen_t Ipv4Addr::NetAttrLen() const
{
	return (socklen_t)sizeof(m_stAddr);
}

const addrinfo* Ipv4Addr::Find(const addrinfo* pList)
{
	for (const addrinfo* pNode = pList; pNode != NULL; pNode = pNode->ai_next)
	{
		if (pNode->ai_family == AF_INET && pNode->ai_addr != NULL && pNode->ai_addrlen >= (socklen_t)sizeof(sockaddr_in))
		{
			return pNode;
		}
	}
	return NULL;
}

Ipv6Addr::Ipv6Addr()
{
	memset(&m_stAddr, 0, sizeof(m_stAddr));
}

static std::string StripIpv6Brackets(const std::string& sAddress)
{
	if (sAddress.size() >= 2 && sAddress.front() == '[' && sAddress.back() == ']')
	{
		return sAddress.substr(1, sAddress.size() - 2);
	}
	return sAddress;
}

bool Ipv6Addr::Init(const std::string& sAddress, uint16_t iPort)
{
	sockaddr_in6 stAddr;
	memset(&stAddr, 0, sizeof(stAddr));
	const std::string sBare = StripIpv6Brackets(sAddress);
	if (inet_pton(AF_INET6, sBare.c_str(), &stAddr.sin6_addr) != 1)
	{
		return false;
	}
	stAddr.sin6_family = AF_INET6;
	stAddr.sin6_port = htons(iPort);
	m_stAddr = stAddr;
	return true;
}

bool Ipv6Addr::Init(const sockaddr* pAddr, socklen_t iLength)
{
	if (pAddr == NULL || pAddr->sa_family != AF_INET6 || iLength < (socklen_t)sizeof(sockaddr_in6))
	{
		return false;
	}
	memcpy(&m_stAddr, pAddr, sizeof(m_stAddr));
	return true;
}

std::string Ipv6Addr::Host() const
{
	char sText[INET6_ADDRSTRLEN];
	memset(sText, 0, sizeof(sText));
	if (inet_ntop(AF_INET6, &m_stAddr.sin6_addr, sText, sizeof(sText)) == NULL)
	{
		return std::string();
	}
	return sText;
}

uint16_t Ipv6Addr::Port() const
{
	return ntohs(m_stAddr.sin6_port);
}

std::string Ipv6Addr::Endpoint() const
{
	const std::string sHost = Host();
	if (sHost.empty())
	{
		return std::string();
	}
	return "[" + sHost + "]:" + std::to_string(ntohs(m_stAddr.sin6_port));
}

bool Ipv6Addr::IsUnspecified() const
{
	return IN6_IS_ADDR_UNSPECIFIED(&m_stAddr.sin6_addr) != 0;
}

sockaddr* Ipv6Addr::NetAttrPtr()
{
	return (sockaddr*)&m_stAddr;
}

socklen_t Ipv6Addr::NetAttrLen() const
{
	return (socklen_t)sizeof(m_stAddr);
}

const addrinfo* Ipv6Addr::Find(const addrinfo* pList)
{
	for (const addrinfo* pNode = pList; pNode != NULL; pNode = pNode->ai_next)
	{
		if (pNode->ai_family == AF_INET6 && pNode->ai_addr != NULL && pNode->ai_addrlen >= (socklen_t)sizeof(sockaddr_in6))
		{
			return pNode;
		}
	}
	return NULL;
}

NetAddr::NetAddr()
    : m_iProtocolType(AF_UNSPEC)
{
}

bool NetAddr::Fill(const std::string& sIp, const std::string& sPort, bool& bBadPort)
{
	bBadPort = false;
	char* pEnd = NULL;
	long iPortNumber = strtol(sPort.c_str(), &pEnd, 10);
	if (sPort.empty() || pEnd == NULL || *pEnd != '\0' || iPortNumber < 0 || iPortNumber > 65535)
	{
		bBadPort = true;
		m_iProtocolType = AF_UNSPEC;
		return false;
	}
	m_iProtocolType = AF_UNSPEC;
	if (m_stV4.Init(sIp, (uint16_t)iPortNumber))
	{
		m_iProtocolType = AF_INET;
		return true;
	}
	if (m_stV6.Init(sIp, (uint16_t)iPortNumber))
	{
		m_iProtocolType = AF_INET6;
		return true;
	}
	return false;
}

bool NetAddr::Init(const std::string& sIp, const std::string& sPort)
{
	bool bBadPort = false;
	if (Fill(sIp, sPort, bBadPort))
	{
		return true;
	}
	if (bBadPort)
	{
		LOG_ERROR("NetAddr::Init bad port=%s", sPort.c_str());
	}
	else
	{
		LOG_ERROR("NetAddr::Init bad ip=%s", sIp.c_str());
	}
	return false;
}

bool NetAddr::Init(SessionManager* pMgr)
{
	if (!Init(pMgr->GetStrIP(), pMgr->GetStrPort()))
	{
		LOG_ERROR("NetAddr::Init id=%s ip=%s port=%s", pMgr->GetIDString().c_str(), pMgr->GetStrIP().c_str(), pMgr->GetStrPort().c_str());
		return false;
	}
	return true;
}

bool NetAddr::Init(const addrinfo* pList)
{
	m_iProtocolType = AF_UNSPEC;
	const addrinfo* pV4 = Ipv4Addr::Find(pList);
	if (pV4 != NULL && m_stV4.Init(pV4->ai_addr, (socklen_t)pV4->ai_addrlen))
	{
		m_iProtocolType = AF_INET;
		return true;
	}
	const addrinfo* pV6 = Ipv6Addr::Find(pList);
	if (pV6 != NULL && m_stV6.Init(pV6->ai_addr, (socklen_t)pV6->ai_addrlen))
	{
		m_iProtocolType = AF_INET6;
		return true;
	}
	return false;
}

bool NetAddr::Init(const sockaddr* pAddr, socklen_t iLength)
{
	m_iProtocolType = AF_UNSPEC;
	if (m_stV4.Init(pAddr, iLength))
	{
		m_iProtocolType = AF_INET;
		return true;
	}
	if (m_stV6.Init(pAddr, iLength))
	{
		m_iProtocolType = AF_INET6;
		return true;
	}
	return false;
}

std::string NetAddr::Host() const
{
	if (m_iProtocolType == AF_INET)
	{
		return m_stV4.Host();
	}
	if (m_iProtocolType == AF_INET6)
	{
		return m_stV6.Host();
	}
	return std::string();
}

uint16_t NetAddr::Port() const
{
	if (m_iProtocolType == AF_INET)
	{
		return m_stV4.Port();
	}
	if (m_iProtocolType == AF_INET6)
	{
		return m_stV6.Port();
	}
	return 0;
}

std::string NetAddr::Endpoint() const
{
	if (m_iProtocolType == AF_INET)
	{
		return m_stV4.Endpoint();
	}
	if (m_iProtocolType == AF_INET6)
	{
		return m_stV6.Endpoint();
	}
	return std::string();
}

std::string NetAddr::Endpoint(const std::string& sIp, const std::string& sPort)
{
	NetAddr stAddr;
	bool bBadPort = false;
	if (stAddr.Fill(sIp, sPort, bBadPort))
	{
		return stAddr.Endpoint();
	}
	return sIp + ":" + sPort;
}

bool NetAddr::IsUnspecified() const
{
	if (m_iProtocolType == AF_INET)
	{
		return m_stV4.IsUnspecified();
	}
	if (m_iProtocolType == AF_INET6)
	{
		return m_stV6.IsUnspecified();
	}
	return false;
}

net_socket_t NetAddr::CreateTcp(bool bNonblock) const
{
	if (m_iProtocolType == AF_INET)
	{
		return NetOs_CreateTcp4(bNonblock);
	}
	if (m_iProtocolType == AF_INET6)
	{
		return NetOs_CreateTcp6(bNonblock);
	}
	errno = EAFNOSUPPORT;
	return INVALID_SOCKET_ID;
}

net_socket_t NetAddr::CreateUdp(bool bNonblock) const
{
	if (m_iProtocolType == AF_INET)
	{
		return NetOs_CreateUdp4(bNonblock);
	}
	if (m_iProtocolType == AF_INET6)
	{
		return NetOs_CreateUdp6(bNonblock);
	}
	errno = EAFNOSUPPORT;
	return INVALID_SOCKET_ID;
}

struct ResolveJob
{
	uv_getaddrinfo_t stRequest;
	std::string sPort;
	void (*fnDone)(void* pArgument, bool bOk, NetAddr stAddr);
	void* pArgument;
};

static void OnResolve(uv_getaddrinfo_t* pRequest, int iStatus, addrinfo* pResponse)
{
	ResolveJob* pJob = static_cast<ResolveJob*>(pRequest->data);
	NetAddr stAddr;
	bool bOk = iStatus == 0 && stAddr.Init(pResponse);
	if (pResponse)
	{
		uv_freeaddrinfo(pResponse);
	}
	if (bOk)
	{
		const std::string sHost = stAddr.Host();
		bOk = !sHost.empty() && stAddr.Init(sHost, pJob->sPort);
	}
	void (*fnDone)(void*, bool, NetAddr) = pJob->fnDone;
	void* pArgument = pJob->pArgument;
	delete pJob;
	fnDone(pArgument, bOk, stAddr);
}

void NetAddr::Resolve(const std::string& sHost, const std::string& sPort, void (*fnDone)(void* pArgument, bool bOk, NetAddr stAddr), void* pArgument)
{
	assert(Poll::GetInstance().InLoopThread());
	NetAddr stLiteral;
	bool bBadPort = false;
	if (stLiteral.Fill(sHost, sPort, bBadPort))
	{
		fnDone(pArgument, true, stLiteral);
		return;
	}
	if (bBadPort)
	{
		fnDone(pArgument, false, NetAddr());
		return;
	}
	ResolveJob* pJob = new ResolveJob();
	pJob->sPort = sPort;
	pJob->fnDone = fnDone;
	pJob->pArgument = pArgument;
	memset(&pJob->stRequest, 0, sizeof(pJob->stRequest));
	pJob->stRequest.data = pJob;
	addrinfo stHints;
	memset(&stHints, 0, sizeof(stHints));
	stHints.ai_family = AF_UNSPEC;
	stHints.ai_socktype = SOCK_STREAM;
	if (uv_getaddrinfo(Poll::GetInstance().Loop(), &pJob->stRequest, OnResolve, sHost.c_str(), NULL, &stHints) != 0)
	{
		delete pJob;
		fnDone(pArgument, false, NetAddr());
	}
}

bool NetAddr::IsValidAttr() const
{
	return m_iProtocolType != AF_UNSPEC;
}

sockaddr* NetAddr::NetAttrPtr()
{
	if (m_iProtocolType == AF_INET)
	{
		return m_stV4.NetAttrPtr();
	}
	if (m_iProtocolType == AF_INET6)
	{
		return m_stV6.NetAttrPtr();
	}
	return NULL;
}

socklen_t NetAddr::NetAttrLen()
{
	if (m_iProtocolType == AF_INET)
	{
		return m_stV4.NetAttrLen();
	}
	if (m_iProtocolType == AF_INET6)
	{
		return m_stV6.NetAttrLen();
	}
	return 0;
}

enum NET_SOCKET_MASK
{
	NSM_INIT = 0x0001,
	NSM_ASYNC = 0x0002,
	NSM_BIND = 0x0004,
	NSM_LISTEN = 0x0008,
	NSM_CONNECT = 0x0010,
	NSM_ACCEPT = 0x0020,
};

NetSocket::NetSocket()
    : m_iSocketDescriptor(INVALID_SOCKET_ID), m_iMask(0)
{
}

bool NetSocket::Init(net_socket_t iSessionId, SessionManager* pMgr)
{
	m_iSocketDescriptor = iSessionId;
	m_iMask |= NSM_INIT;
	return m_stAddress.Init(pMgr);
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

void NetSocket::SetAsync(bool bAsync)
{
	if (bAsync)
	{
		m_iMask |= NSM_ASYNC;
	}
	else
	{
		m_iMask &= ~NSM_ASYNC;
	}
}

bool NetSocket::CreateSocket(SessionManager* pMgr, bool bAsync)
{
	return NetHelper::CreateSocket(*this, pMgr, bAsync);
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
}
}