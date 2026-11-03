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

#define INVALID_SOCKET_ID ((net_socket_t)-1)

void NetOs_Startup();
net_socket_t NetOs_CreateTcp(bool bNonblockParam);
int NetOs_SetNonblock(net_socket_t iDescriptorParam);
// 关掉 Nagle。小包一来一回的请求不等凑包
int NetOs_SetNoDelay(net_socket_t iDescriptorParam);
int NetOs_Close(net_socket_t iDescriptorParam);
int NetOs_Recv(net_socket_t iDescriptorParam, void* pBufferParam, size_t iLengthParam);
int NetOs_Send(net_socket_t iDescriptorParam, const void* pBufferParam, size_t iLengthParam);
net_socket_t NetOs_Accept(net_socket_t iDescriptorParam);
net_socket_t NetOs_Dup(net_socket_t iDescriptorParam);
int NetOs_Bind(net_socket_t iDescriptorParam, const sockaddr* pAddrParam, int iLengthParam);
int NetOs_Listen(net_socket_t iDescriptorParam, int iBacklogParam);
int NetOs_Connect(net_socket_t iDescriptorParam, const sockaddr* pAddrParam, int iLengthParam);
int NetOs_GetSocketError(net_socket_t iDescriptorParam);

inline const char* NetOs_StrError(int iErrorParam)
{
	static thread_local char vBuffer[128];
#ifdef _WIN32
	if (strerror_s(buf, sizeof(buf), err) != 0)
	{
		buf[0] = '\0';
	}
#else
	const char* pText = strerror(iErrorParam);
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
