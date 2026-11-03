#include "oi_net_os.h"
#include <string.h>

#ifdef _WIN32
static void NetOs_MapError(int iWsaErrorParam)
{
	if (iWsaErrorParam == WSAEWOULDBLOCK || iWsaErrorParam == WSAEINPROGRESS)
	{
		errno = EINPROGRESS;
	}
	else if (iWsaErrorParam == WSAEINTR)
	{
		errno = EINTR;
	}
	else
	{
		errno = iWsaErrorParam;
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

net_socket_t NetOs_CreateTcp(bool bNonblockParam)
{
	NetOs_Startup();
#ifdef _WIN32
	SOCKET iDescriptorLocal = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (iDescriptorLocal == INVALID_SOCKET)
	{
		NetOs_MapError(WSAGetLastError());
		return INVALID_SOCKET_ID;
	}
	if (bNonblockParam)
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
	if (bNonblockParam)
	{
		iFlags |= SOCK_NONBLOCK;
	}
#endif
	int iDescriptorLocal = socket(AF_INET, iFlags, 0);
	if (iDescriptorLocal < 0)
	{
		return INVALID_SOCKET_ID;
	}
#ifndef SOCK_NONBLOCK
	if (bNonblockParam)
	{
		NetOs_SetNonblock(iDescriptorLocal);
	}
#endif
	return iDescriptorLocal;
#endif
}

int NetOs_SetNonblock(net_socket_t iDescriptorParam)
{
#ifdef _WIN32
	u_long iMode = 1;
	if (ioctlsocket((SOCKET)iDescriptorParam, FIONBIO, &iMode) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	int iFlags = fcntl(iDescriptorParam, F_GETFL, 0);
	if (iFlags < 0)
	{
		return -1;
	}
	return fcntl(iDescriptorParam, F_SETFL, iFlags | O_NONBLOCK);
#endif
}

int NetOs_SetNoDelay(net_socket_t iDescriptorParam)
{
	int iOn = 1;
#ifdef _WIN32
	if (setsockopt((SOCKET)iDescriptorParam, IPPROTO_TCP, TCP_NODELAY, (const char*)&iOn, sizeof(iOn)) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	return setsockopt((int)iDescriptorParam, IPPROTO_TCP, TCP_NODELAY, &iOn, sizeof(iOn));
#endif
}

int NetOs_Close(net_socket_t iDescriptorParam)
{
	if (iDescriptorParam == INVALID_SOCKET_ID)
	{
		return 0;
	}
#ifdef _WIN32
	return closesocket((SOCKET)iDescriptorParam);
#else
	return close((int)iDescriptorParam);
#endif
}

int NetOs_Recv(net_socket_t iDescriptorParam, void* pBufferParam, size_t iLengthParam)
{
#ifdef _WIN32
	int iCount = recv((SOCKET)iDescriptorParam, (char*)pBufferParam, (int)iLengthParam, 0);
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
	return (int)read((int)iDescriptorParam, pBufferParam, iLengthParam);
#endif
}

int NetOs_Send(net_socket_t iDescriptorParam, const void* pBufferParam, size_t iLengthParam)
{
#ifdef _WIN32
	int iCount = send((SOCKET)iDescriptorParam, (const char*)pBufferParam, (int)iLengthParam, 0);
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
	return (int)write((int)iDescriptorParam, pBufferParam, iLengthParam);
#endif
}

net_socket_t NetOs_Accept(net_socket_t iDescriptorParam)
{
#ifdef _WIN32
	SOCKET iClient = accept((SOCKET)iDescriptorParam, NULL, NULL);
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
	int iClient = accept((int)iDescriptorParam, 0, 0);
	if (iClient >= 0)
	{
		NetOs_SetNonblock(iClient);
	}
	return iClient;
#endif
}

net_socket_t NetOs_Dup(net_socket_t iDescriptorParam)
{
#ifdef _WIN32
	WSAPROTOCOL_INFOW stInfo;
	memset(&stInfo, 0, sizeof(stInfo));
	if (WSADuplicateSocketW((SOCKET)iDescriptorParam, GetCurrentProcessId(), &stInfo) != 0)
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
	return dup((int)iDescriptorParam);
#endif
}

int NetOs_Bind(net_socket_t iDescriptorParam, const sockaddr* pAddrParam, int iLengthParam)
{
	// 监听口关掉之后，旧连接还会在 TIME_WAIT 里占着这个端口。允许复用，重启才能再绑上。
	int iOn = 1;
#ifdef _WIN32
	if (setsockopt((SOCKET)iDescriptorParam, SOL_SOCKET, SO_REUSEADDR, (const char*)&iOn, sizeof(iOn)) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	if (bind((SOCKET)iDescriptorParam, pAddrParam, iLengthParam) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	if (setsockopt((int)iDescriptorParam, SOL_SOCKET, SO_REUSEADDR, &iOn, sizeof(iOn)) != 0)
	{
		return -1;
	}
	return bind((int)iDescriptorParam, pAddrParam, iLengthParam);
#endif
}

int NetOs_Listen(net_socket_t iDescriptorParam, int iBacklogParam)
{
#ifdef _WIN32
	if (listen((SOCKET)iDescriptorParam, iBacklogParam) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	return listen((int)iDescriptorParam, iBacklogParam);
#endif
}

int NetOs_Connect(net_socket_t iDescriptorParam, const sockaddr* pAddrParam, int iLengthParam)
{
#ifdef _WIN32
	if (connect((SOCKET)iDescriptorParam, pAddrParam, iLengthParam) == 0)
	{
		return 0;
	}
	NetOs_MapError(WSAGetLastError());
	return -1;
#else
	return connect((int)iDescriptorParam, pAddrParam, iLengthParam);
#endif
}

int NetOs_GetSocketError(net_socket_t iDescriptorParam)
{
	int iValue = 0;
#ifdef _WIN32
	int iLength = sizeof(iValue);
	if (getsockopt((SOCKET)iDescriptorParam, SOL_SOCKET, SO_ERROR, (char*)&iValue, &iLength) != 0)
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
	if (getsockopt((int)iDescriptorParam, SOL_SOCKET, SO_ERROR, &iValue, &iLength) != 0)
	{
		return errno;
	}
	return iValue;
#endif
}
