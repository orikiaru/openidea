#include "oi_net_os.h"
#include <string.h>

#ifdef _WIN32
static void NetOs_MapError(int iWsaError)
{
	if (iWsaError == WSAEWOULDBLOCK || iWsaError == WSAEINPROGRESS)
	{
		errno = EINPROGRESS;
	}
	else if (iWsaError == WSAEINTR)
	{
		errno = EINTR;
	}
	else
	{
		errno = iWsaError;
	}
}
#endif

void NetOs_Startup()
{
#ifdef _WIN32
	static bool bStarted = false;
	if (bStarted)
	{
		return;
	}
	WSADATA stData;
	WSAStartup(MAKEWORD(2, 2), &stData);
	bStarted = true;
#endif
}

static net_socket_t NetOs_OpenTcp(int iFamily, bool bNonblock)
{
	NetOs_Startup();
#ifdef _WIN32
	SOCKET iDescriptorLocal = socket(iFamily, SOCK_STREAM, IPPROTO_TCP);
	if (iDescriptorLocal == INVALID_SOCKET)
	{
		NetOs_MapError(WSAGetLastError());
		return INVALID_SOCKET_ID;
	}
	if (bNonblock)
	{
		NetOs_SetNonblock((net_socket_t)iDescriptorLocal);
	}
	return (net_socket_t)iDescriptorLocal;
#else
	int iFlags = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
	iFlags |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
	if (bNonblock)
	{
		iFlags |= SOCK_NONBLOCK;
	}
#endif
	int iDescriptorLocal = socket(iFamily, iFlags, 0);
	if (iDescriptorLocal < 0)
	{
		return INVALID_SOCKET_ID;
	}
#ifndef SOCK_NONBLOCK
	if (bNonblock)
	{
		NetOs_SetNonblock(iDescriptorLocal);
	}
#endif
	return iDescriptorLocal;
#endif
}

static int NetOs_SetV6Only(net_socket_t iDescriptor)
{
	int iOnly = 1;
#ifdef _WIN32
	if (setsockopt((SOCKET)iDescriptor, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&iOnly, sizeof(iOnly)) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	return setsockopt((int)iDescriptor, IPPROTO_IPV6, IPV6_V6ONLY, &iOnly, sizeof(iOnly));
#endif
}

net_socket_t NetOs_CreateTcp4(bool bNonblock)
{
	return NetOs_OpenTcp(AF_INET, bNonblock);
}

net_socket_t NetOs_CreateTcp6(bool bNonblock)
{
	net_socket_t iDescriptor = NetOs_OpenTcp(AF_INET6, bNonblock);
	if (iDescriptor == INVALID_SOCKET_ID)
	{
		return INVALID_SOCKET_ID;
	}
	if (NetOs_SetV6Only(iDescriptor) != 0)
	{
		NetOs_Close(iDescriptor);
		return INVALID_SOCKET_ID;
	}
	return iDescriptor;
}

int NetOs_SetNonblock(net_socket_t iDescriptor)
{
#ifdef _WIN32
	u_long iMode = 1;
	if (ioctlsocket((SOCKET)iDescriptor, FIONBIO, &iMode) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	int iFlags = fcntl(iDescriptor, F_GETFL, 0);
	if (iFlags < 0)
	{
		return -1;
	}
	return fcntl(iDescriptor, F_SETFL, iFlags | O_NONBLOCK);
#endif
}

int NetOs_SetNoDelay(net_socket_t iDescriptor)
{
	int iOn = 1;
#ifdef _WIN32
	if (setsockopt((SOCKET)iDescriptor, IPPROTO_TCP, TCP_NODELAY, (const char*)&iOn, sizeof(iOn)) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	return setsockopt((int)iDescriptor, IPPROTO_TCP, TCP_NODELAY, &iOn, sizeof(iOn));
#endif
}

int NetOs_Close(net_socket_t iDescriptor)
{
	if (iDescriptor == INVALID_SOCKET_ID)
	{
		return 0;
	}
#ifdef _WIN32
	return closesocket((SOCKET)iDescriptor);
#else
	return close((int)iDescriptor);
#endif
}

int NetOs_Recv(net_socket_t iDescriptor, void* pBuffer, size_t iLength)
{
#ifdef _WIN32
	int iCount = recv((SOCKET)iDescriptor, (char*)pBuffer, (int)iLength, 0);
	if (iCount == SOCKET_ERROR)
	{
		int iError = WSAGetLastError();
		if (iError == WSAEWOULDBLOCK)
		{
			errno = EAGAIN;
		}
		else if (iError == WSAEINTR)
		{
			errno = EINTR;
		}
		else
		{
			errno = iError;
		}
		return -1;
	}
	return iCount;
#else
	return (int)read((int)iDescriptor, pBuffer, iLength);
#endif
}

int NetOs_Send(net_socket_t iDescriptor, const void* pBuffer, size_t iLength)
{
#ifdef _WIN32
	int iCount = send((SOCKET)iDescriptor, (const char*)pBuffer, (int)iLength, 0);
	if (iCount == SOCKET_ERROR)
	{
		int iError = WSAGetLastError();
		if (iError == WSAEWOULDBLOCK)
		{
			errno = EAGAIN;
		}
		else if (iError == WSAEINTR)
		{
			errno = EINTR;
		}
		else
		{
			errno = iError;
		}
		return -1;
	}
	return iCount;
#else
	return (int)write((int)iDescriptor, pBuffer, iLength);
#endif
}

net_socket_t NetOs_Accept(net_socket_t iDescriptor)
{
#ifdef _WIN32
	SOCKET iClient = accept((SOCKET)iDescriptor, NULL, NULL);
	if (iClient == INVALID_SOCKET)
	{
		int iError = WSAGetLastError();
		if (iError == WSAEWOULDBLOCK)
		{
			errno = EAGAIN;
		}
		else if (iError == WSAEINTR)
		{
			errno = EINTR;
		}
		else
		{
			errno = iError;
		}
		return INVALID_SOCKET_ID;
	}
	NetOs_SetNonblock((net_socket_t)iClient);
	return (net_socket_t)iClient;
#else
	int iClient = accept((int)iDescriptor, 0, 0);
	if (iClient >= 0)
	{
		NetOs_SetNonblock(iClient);
	}
	return iClient;
#endif
}

net_socket_t NetOs_Dup(net_socket_t iDescriptor)
{
#ifdef _WIN32
	WSAPROTOCOL_INFOW stInfo;
	memset(&stInfo, 0, sizeof(stInfo));
	if (WSADuplicateSocketW((SOCKET)iDescriptor, GetCurrentProcessId(), &stInfo) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return INVALID_SOCKET_ID;
	}
	SOCKET iDuplicateDescriptor = WSASocketW(stInfo.iAddressFamily, stInfo.iSocketType, stInfo.iProtocol, &stInfo, 0, WSA_FLAG_OVERLAPPED);
	if (iDuplicateDescriptor == INVALID_SOCKET)
	{
		NetOs_MapError(WSAGetLastError());
		return INVALID_SOCKET_ID;
	}
	NetOs_SetNonblock((net_socket_t)iDuplicateDescriptor);
	return (net_socket_t)iDuplicateDescriptor;
#else
	return dup((int)iDescriptor);
#endif
}

int NetOs_Bind(net_socket_t iDescriptor, const sockaddr* pAddr, int iLength)
{
	// 监听口关掉之后，旧连接还会在 TIME_WAIT 里占着这个端口。允许复用，重启才能再绑上。
	int iOn = 1;
#ifdef _WIN32
	if (setsockopt((SOCKET)iDescriptor, SOL_SOCKET, SO_REUSEADDR, (const char*)&iOn, sizeof(iOn)) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	if (bind((SOCKET)iDescriptor, pAddr, iLength) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	if (setsockopt((int)iDescriptor, SOL_SOCKET, SO_REUSEADDR, &iOn, sizeof(iOn)) != 0)
	{
		return -1;
	}
	return bind((int)iDescriptor, pAddr, iLength);
#endif
}

int NetOs_Listen(net_socket_t iDescriptor, int iBacklog)
{
#ifdef _WIN32
	if (listen((SOCKET)iDescriptor, iBacklog) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	return listen((int)iDescriptor, iBacklog);
#endif
}

int NetOs_Connect(net_socket_t iDescriptor, const sockaddr* pAddr, int iLength)
{
#ifdef _WIN32
	if (connect((SOCKET)iDescriptor, pAddr, iLength) == 0)
	{
		return 0;
	}
	NetOs_MapError(WSAGetLastError());
	return -1;
#else
	return connect((int)iDescriptor, pAddr, iLength);
#endif
}

int NetOs_GetSocketError(net_socket_t iDescriptor)
{
	int iValue = 0;
#ifdef _WIN32
	int iLength = sizeof(iValue);
	if (getsockopt((SOCKET)iDescriptor, SOL_SOCKET, SO_ERROR, (char*)&iValue, &iLength) != 0)
	{
		return WSAGetLastError();
	}
	if (iValue == WSAEWOULDBLOCK || iValue == WSAEINPROGRESS)
	{
		return EINPROGRESS;
	}
	if (iValue == WSAEINTR)
	{
		return EINTR;
	}
	return iValue;
#else
	socklen_t iLength = sizeof(iValue);
	if (getsockopt((int)iDescriptor, SOL_SOCKET, SO_ERROR, &iValue, &iLength) != 0)
	{
		return errno;
	}
	return iValue;
#endif
}
