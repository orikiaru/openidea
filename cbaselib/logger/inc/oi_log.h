#pragma once

#include <stddef.h>
#include "oi_header.h"
#define _CHECK_LOG_FORMAT_
#ifdef _CHECK_LOG_FORMAT_
#define CHECK_FORMAT(string_index, first_to_check) __attribute__((format(printf, string_index, first_to_check)))
#else
#define CHECK_FORMAT(string_index, first_to_check)
#endif

namespace OI
{
namespace Log
{
bool Init();
// 日志服自己用：不连日志服，整行日志交给 sink 写本地。传 NULL 摘掉，之后只打 stdout。
typedef bool (*LocalSink)(LOG_LEVEL iLevelParam, const char* pTextParam, size_t iLengthParam);
void InitLocal(LocalSink fnSinkParam);
bool Log(LOG_LEVEL iLevelParam, const char* pFormatParam, ...) __attribute__((format(printf, 2, 3)));
}
}

#define LOG_WRAPPER(lv, ...) ::OI::Log::Log(lv, __VA_ARGS__)
#define LOG_TRACE(...) ::OI::Log::Log(LOG_LEVEL::LOG_TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) ::OI::Log::Log(LOG_LEVEL::LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) ::OI::Log::Log(LOG_LEVEL::LOG_INFO, __VA_ARGS__)
#define LOG_WARNING(...) ::OI::Log::Log(LOG_LEVEL::LOG_WARNING, __VA_ARGS__)
#define LOG_ERROR(...) ::OI::Log::Log(LOG_LEVEL::LOG_ERROR, __VA_ARGS__)
