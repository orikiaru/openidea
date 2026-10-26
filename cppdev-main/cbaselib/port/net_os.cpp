#include "net_os.h"
#include <string.h>

#ifdef _WIN32
static void NetOs_MapError(int wsa_error)
{
	if (wsa_error == WSAEWOULDBLOCK || wsa_error == WSAEINPROGRESS)
	{
		errno = EINPROGRESS;
	}
	else if (wsa_error == WSAEINTR)
	{
		errno = EINTR;
	}
	else
	{
		errno = wsa_error;
	}
}
#endif

void NetOs_Startup()
{
#ifdef _WIN32
	static bool started = false;
	if (started)
	{
		return;
	}
	WSADATA data;
	WSAStartup(MAKEWORD(2, 2), &data);
	started = true;
#endif
}

net_socket_t NetOs_CreateTcp(bool nonblock)
{
	NetOs_Startup();
#ifdef _WIN32
	SOCKET fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET)
	{
		NetOs_MapError(WSAGetLastError());
		return INVALID_SOCKET_ID;
	}
	if (nonblock)
	{
		NetOs_SetNonblock((net_socket_t)fd);
	}
	return (net_socket_t)fd;
#else
	int flags = SOCK_STREAM;
#ifdef SOCK_CLOEXEC
	flags |= SOCK_CLOEXEC;
#endif
#ifdef SOCK_NONBLOCK
	if (nonblock)
	{
		flags |= SOCK_NONBLOCK;
	}
#endif
	int fd = socket(AF_INET, flags, 0);
	if (fd < 0)
	{
		return INVALID_SOCKET_ID;
	}
#ifndef SOCK_NONBLOCK
	if (nonblock)
	{
		NetOs_SetNonblock(fd);
	}
#endif
	return fd;
#endif
}

int NetOs_SetNonblock(net_socket_t fd)
{
#ifdef _WIN32
	u_long mode = 1;
	if (ioctlsocket((SOCKET)fd, FIONBIO, &mode) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
	{
		return -1;
	}
	return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

int NetOs_Close(net_socket_t fd)
{
	if (fd == INVALID_SOCKET_ID)
	{
		return 0;
	}
#ifdef _WIN32
	return closesocket((SOCKET)fd);
#else
	return close((int)fd);
#endif
}

int NetOs_Recv(net_socket_t fd, void* buf, size_t len)
{
#ifdef _WIN32
	int n = recv((SOCKET)fd, (char*)buf, (int)len, 0);
	if (n == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK)
		{
			errno = EAGAIN;
		}
		else if (err == WSAEINTR)
		{
			errno = EINTR;
		}
		else
		{
			errno = err;
		}
		return -1;
	}
	return n;
#else
	return (int)read((int)fd, buf, len);
#endif
}

int NetOs_Send(net_socket_t fd, const void* buf, size_t len)
{
#ifdef _WIN32
	int n = send((SOCKET)fd, (const char*)buf, (int)len, 0);
	if (n == SOCKET_ERROR)
	{
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK)
		{
			errno = EAGAIN;
		}
		else if (err == WSAEINTR)
		{
			errno = EINTR;
		}
		else
		{
			errno = err;
		}
		return -1;
	}
	return n;
#else
	return (int)write((int)fd, buf, len);
#endif
}

net_socket_t NetOs_Accept(net_socket_t fd)
{
#ifdef _WIN32
	SOCKET client = accept((SOCKET)fd, NULL, NULL);
	if (client == INVALID_SOCKET)
	{
		int err = WSAGetLastError();
		if (err == WSAEWOULDBLOCK)
		{
			errno = EAGAIN;
		}
		else if (err == WSAEINTR)
		{
			errno = EINTR;
		}
		else
		{
			errno = err;
		}
		return INVALID_SOCKET_ID;
	}
	NetOs_SetNonblock((net_socket_t)client);
	return (net_socket_t)client;
#else
	int client = accept((int)fd, 0, 0);
	if (client >= 0)
	{
		NetOs_SetNonblock(client);
	}
	return client;
#endif
}

net_socket_t NetOs_Dup(net_socket_t fd)
{
#ifdef _WIN32
	WSAPROTOCOL_INFOA info;
	memset(&info, 0, sizeof(info));
	if (WSADuplicateSocketA((SOCKET)fd, GetCurrentProcessId(), &info) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return INVALID_SOCKET_ID;
	}
	SOCKET dup_fd = WSASocketA(info.iAddressFamily, info.iSocketType, info.iProtocol, &info, 0, WSA_FLAG_OVERLAPPED);
	if (dup_fd == INVALID_SOCKET)
	{
		NetOs_MapError(WSAGetLastError());
		return INVALID_SOCKET_ID;
	}
	NetOs_SetNonblock((net_socket_t)dup_fd);
	return (net_socket_t)dup_fd;
#else
	return dup((int)fd);
#endif
}

int NetOs_Bind(net_socket_t fd, const sockaddr* addr, int len)
{
#ifdef _WIN32
	if (bind((SOCKET)fd, addr, len) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	return bind((int)fd, addr, len);
#endif
}

int NetOs_Listen(net_socket_t fd, int backlog)
{
#ifdef _WIN32
	if (listen((SOCKET)fd, backlog) != 0)
	{
		NetOs_MapError(WSAGetLastError());
		return -1;
	}
	return 0;
#else
	return listen((int)fd, backlog);
#endif
}

int NetOs_Connect(net_socket_t fd, const sockaddr* addr, int len)
{
#ifdef _WIN32
	if (connect((SOCKET)fd, addr, len) == 0)
	{
		return 0;
	}
	NetOs_MapError(WSAGetLastError());
	return -1;
#else
	return connect((int)fd, addr, len);
#endif
}

int NetOs_GetSocketError(net_socket_t fd)
{
	int value = 0;
#ifdef _WIN32
	int len = sizeof(value);
	if (getsockopt((SOCKET)fd, SOL_SOCKET, SO_ERROR, (char*)&value, &len) != 0)
	{
		return WSAGetLastError();
	}
	if (value == WSAEWOULDBLOCK || value == WSAEINPROGRESS)
	{
		return EINPROGRESS;
	}
	if (value == WSAEINTR)
	{
		return EINTR;
	}
	return value;
#else
	socklen_t len = sizeof(value);
	if (getsockopt((int)fd, SOL_SOCKET, SO_ERROR, &value, &len) != 0)
	{
		return errno;
	}
	return value;
#endif
}
