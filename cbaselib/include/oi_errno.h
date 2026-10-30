#pragma once
#include <stdint.h>

// 框架错误码。0 成功，负数失败。正数留给系统 errno 和业务。

enum OI_ERR
{
	OI_OK = 0,
	OI_ERR_FAIL = -1,		// 发送失败或解包失败
	OI_ERR_TIMEOUT = -2,	// 已经发出之后超时
	OI_ERR_CONNECT = -3,	// 连接失败，或连接断开
	OI_ERR_EXIST = -4,		// 已被占用，例如同名同 id 的服务已经在线
};

#define NET_PARSER_ERR_BEGIN (-10000)

enum PARSER_ERR_ENUM
{
	PARSER_ERR_SUCCESS = 0,
	PARSER_ERR_UNFINISH = NET_PARSER_ERR_BEGIN,
	PARSER_ERR_PROTOCOL_TYPE,
	PARSER_ERR_PROTOCOL_SIZE,
	PARSER_ERR_OTHER,
};

#define SESSION_MANAGER_ERR_BEGIN (-20000)
enum
{
	SESSION_MANAGER_NORMAL_CLOSE = SESSION_MANAGER_ERR_BEGIN,
};
