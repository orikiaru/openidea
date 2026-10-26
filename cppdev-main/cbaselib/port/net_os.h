#pragma once
#include <stdint.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <errno.h>
typedef intptr_t net_socket_t;
#ifndef EINPROGRESS
#define EINPROGRESS 112
#endif
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
typedef int net_socket_t;
#endif

#define INVALID_SOCKET_ID ((net_socket_t)-1)

void NetOs_Startup();
net_socket_t NetOs_CreateTcp(bool nonblock);
int NetOs_SetNonblock(net_socket_t fd);
int NetOs_Close(net_socket_t fd);
int NetOs_Recv(net_socket_t fd, void* buf, size_t len);
int NetOs_Send(net_socket_t fd, const void* buf, size_t len);
net_socket_t NetOs_Accept(net_socket_t fd);
net_socket_t NetOs_Dup(net_socket_t fd);
int NetOs_Bind(net_socket_t fd, const sockaddr* addr, int len);
int NetOs_Listen(net_socket_t fd, int backlog);
int NetOs_Connect(net_socket_t fd, const sockaddr* addr, int len);
int NetOs_GetSocketError(net_socket_t fd);
