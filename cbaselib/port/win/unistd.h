#pragma once
#ifdef _WIN32
#include <io.h>
#include <direct.h>
#include <process.h>
#include <stdio.h>
#include <time.h>
#include <windows.h>
#ifndef F_OK
#define F_OK 0
#endif
inline int access(const char* path, int)
{
	return _access(path, 0);
}
inline int mkdir(const char* path, int)
{
	return _mkdir(path);
}
inline int usleep(unsigned int usec)
{
	Sleep((usec + 999u) / 1000u);
	return 0;
}
#ifndef CPPDEV_HAVE_LOCALTIME_R
#define CPPDEV_HAVE_LOCALTIME_R
inline struct tm* localtime_r(const time_t* clock, struct tm* result)
{
	return localtime_s(result, clock) == 0 ? result : NULL;
}
#endif
#endif
