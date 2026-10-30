#ifndef __LOGGER_H_
#define __LOGGER_H_

#include "oi_header.h"
#define _CHECK_LOG_FORMAT_  
#ifdef _CHECK_LOG_FORMAT_
#define CHECK_FORMAT(string_index, first_to_check) __attribute__((format(printf, string_index, first_to_check)))
#else
#define CHECK_FORMAT(string_index, first_to_check)
#endif



namespace OI { namespace Log
{
	bool Init();
	bool Log(LOG_LEVEL lv, const char* format, ...)__attribute__((format(printf, 2, 3)));
}}

#define LOG_WRAPPER(lv, ...)	::OI::Log::Log(lv,						__VA_ARGS__)
#define LOG_TRACE(...)			::OI::Log::Log(LOG_LEVEL::LOG_TRACE,		__VA_ARGS__)
#define LOG_DEBUG(...)			::OI::Log::Log(LOG_LEVEL::LOG_DEBUG,		__VA_ARGS__)
#define LOG_INFO(...)			::OI::Log::Log(LOG_LEVEL::LOG_INFO,			__VA_ARGS__)
#define LOG_LOG_WARNING(...)	::OI::Log::Log(LOG_LEVEL::LOG_WARNING,	__VA_ARGS__)
#define LOG_ERROR(...)			::OI::Log::Log(LOG_LEVEL::LOG_ERROR,		__VA_ARGS__)



#endif /*__LOGGER_H_*/