#pragma once
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <windows.h>
#include <time.h>
typedef long suseconds_t;
#ifndef CPPDEV_HAVE_LOCALTIME_R
#define CPPDEV_HAVE_LOCALTIME_R
inline struct tm* localtime_r(const time_t* clock, struct tm* result)
{
	return localtime_s(result, clock) == 0 ? result : NULL;
}
#endif
struct itimerval
{
	struct timeval it_interval;
	struct timeval it_value;
};
#ifndef ITIMER_REAL
#define ITIMER_REAL 0
#endif
inline int setitimer(int, const struct itimerval*, struct itimerval*)
{
	return 0;
}
inline int gettimeofday(struct timeval* tv, void*)
{
	FILETIME file_time;
	GetSystemTimeAsFileTime(&file_time);
	ULARGE_INTEGER ticks;
	ticks.LowPart = file_time.dwLowDateTime;
	ticks.HighPart = file_time.dwHighDateTime;
	const unsigned long long epoch = 116444736000000000ULL;
	unsigned long long usec = (ticks.QuadPart - epoch) / 10ULL;
	tv->tv_sec = (long)(usec / 1000000ULL);
	tv->tv_usec = (long)(usec % 1000000ULL);
	return 0;
}
#endif
