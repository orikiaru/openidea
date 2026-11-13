#pragma once

#include <stddef.h>
#include <string_view>
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
// 错误文本只转义控制字符，不把外部换行/NUL 当成日志边界；超长文本明确标记截断。
// 不分配堆内存，异常处理路径也可使用。调用方不要传请求正文或凭据。
class DiagnosticText
{
  public:
	explicit DiagnosticText(std::string_view sText) noexcept;
	// 转义后的文本。
	const char* GetText() const noexcept
	{
		return m_vText;
	}
	// 原文是否被截断。
	bool IsTruncated() const noexcept
	{
		return m_bTruncated;
	}

  private:
	// 转义后的文本，末尾带结束符。最长 4096 个可见字符。
	char m_vText[4097]{};
	// 原文超长，文本里已经标了截断。
	bool m_bTruncated = false;
};

// 按配置初始化日志客户端。返回框架错误码。
int32_t Init();
// 日志服自己用：不连日志服，整行日志交给 sink 写本地。传 NULL 摘掉，之后只打 stdout。
typedef int32_t (*LocalSink)(LOG_LEVEL iLevel, const char* pText, size_t iLength);
void InitLocal(LocalSink fnSink);
// 按级别格式化一条日志。返回框架错误码。
int32_t Log(LOG_LEVEL iLevel, const char* pFormat, ...) __attribute__((format(printf, 2, 3)));
}
}

#define LOG_WRAPPER(lv, ...) ::OI::Log::Log(lv, __VA_ARGS__)
#define LOG_TRACE(...) ::OI::Log::Log(LOG_LEVEL::LOG_TRACE, __VA_ARGS__)
#define LOG_DEBUG(...) ::OI::Log::Log(LOG_LEVEL::LOG_DEBUG, __VA_ARGS__)
#define LOG_INFO(...) ::OI::Log::Log(LOG_LEVEL::LOG_INFO, __VA_ARGS__)
#define LOG_WARNING(...) ::OI::Log::Log(LOG_LEVEL::LOG_WARNING, __VA_ARGS__)
#define LOG_ERROR(...) ::OI::Log::Log(LOG_LEVEL::LOG_ERROR, __VA_ARGS__)
