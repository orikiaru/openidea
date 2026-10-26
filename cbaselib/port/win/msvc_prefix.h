#pragma once
#ifdef _MSC_VER
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifndef __attribute__
#define __attribute__(x)
#endif
#ifndef SIGPIPE
#define SIGPIPE 13
#endif
#ifndef SIGHUP
#define SIGHUP 1
#endif
#ifndef SIGQUIT
#define SIGQUIT 3
#endif
#ifndef EINPROGRESS
#define EINPROGRESS 112
#endif
#endif
