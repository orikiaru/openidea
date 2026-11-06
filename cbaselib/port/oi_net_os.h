#pragma once
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <errno.h>
#include <string.h>
typedef intptr_t net_socket_t;
#ifndef EINPROGRESS
#define EINPROGRESS 112
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
typedef int net_socket_t;
#endif

#define INVALID_SOCKET_ID ((net_socket_t) - 1)

void NetOs_Startup();
// IPv4 TCP 套接字。
net_socket_t NetOs_CreateTcp4(bool bNonblock);
// IPv6 TCP 套接字。打开 IPV6_V6ONLY，不接 IPv4 映射地址。
net_socket_t NetOs_CreateTcp6(bool bNonblock);
// IPv4 UDP 套接字。Windows 上关掉 ICMP 端口不可达引起的 WSAECONNRESET。
net_socket_t NetOs_CreateUdp4(bool bNonblock);
// IPv6 UDP 套接字。打开 IPV6_V6ONLY。
net_socket_t NetOs_CreateUdp6(bool bNonblock);
int NetOs_SetNonblock(net_socket_t iDescriptor);
// 关掉 Nagle。小包一来一回的请求不等凑包
int NetOs_SetNoDelay(net_socket_t iDescriptor);
int NetOs_Close(net_socket_t iDescriptor);
int NetOs_Recv(net_socket_t iDescriptor, void* pBuffer, size_t iLength);
int NetOs_Send(net_socket_t iDescriptor, const void* pBuffer, size_t iLength);
// 收一包，带回来源地址。没有包时返回 -1，errno 为 EAGAIN。
int NetOs_RecvFrom(net_socket_t iDescriptor, void* pBuffer, size_t iLength, sockaddr* pAddr, socklen_t* pAddrLength);
// 发一包到这个地址。
int NetOs_SendTo(net_socket_t iDescriptor, const void* pBuffer, size_t iLength, const sockaddr* pAddr, socklen_t iAddrLength);
// 套接字绑上的本端地址。
int NetOs_GetSockName(net_socket_t iDescriptor, sockaddr* pAddr, socklen_t* pAddrLength);
net_socket_t NetOs_Accept(net_socket_t iDescriptor);
net_socket_t NetOs_Dup(net_socket_t iDescriptor);
int NetOs_Bind(net_socket_t iDescriptor, const sockaddr* pAddr, int iLength);
// UDP 绑定。UDP 没有 TIME_WAIT，不开 SO_REUSEADDR，免得两个进程绑上同一个口、包被后来的抢走。
int NetOs_BindDatagram(net_socket_t iDescriptor, const sockaddr* pAddr, int iLength);
int NetOs_Listen(net_socket_t iDescriptor, int iBacklog);
int NetOs_Connect(net_socket_t iDescriptor, const sockaddr* pAddr, int iLength);
int NetOs_GetSocketError(net_socket_t iDescriptor);

inline const char* NetOs_StrError(int iError)
{
	static thread_local char vBuffer[128];
#ifdef _WIN32
	if (strerror_s(vBuffer, sizeof(vBuffer), iError) != 0)
	{
		vBuffer[0] = '\0';
	}
#else
	const char* pText = strerror(iError);
	if (!pText)
	{
		pText = "";
	}
	size_t iNumber = 0;
	while (pText[iNumber] && iNumber + 1 < sizeof(vBuffer))
	{
		vBuffer[iNumber] = pText[iNumber];
		iNumber++;
	}
	vBuffer[iNumber] = '\0';
#endif
	return vBuffer;
}
