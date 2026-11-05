#pragma once
#include <stdint.h>

// 框架错误码。0 成功，负数失败。正数留给系统 errno 和业务。

enum OI_ERR
{
	OI_OK = 0,
	OI_ERR_FAIL = -1,    // 发送失败或解包失败
	OI_ERR_TIMEOUT = -2, // 已经发出之后超时
	OI_ERR_CONNECT = -3, // 连接失败，或连接断开
	OI_ERR_BUSY = -5,    // 等待队列达到容量上限
	OI_ERR_EXIST = -4,   // 已被占用，例如同名同 id 的服务已经在线
	OI_ERR_INVALID_ARGUMENT = -6,
	OI_ERR_NOT_FOUND = -7,
	OI_ERR_REDIS = -8,           // Redis 服务端返回错误，原生回执保留错误文本
	OI_ERR_REDIS_WRONGTYPE = -9, // key 的 Redis 数据类型不匹配
	OI_ERR_PROTOCOL = -10,       // 回执形状不符合命令约定
	OI_ERR_DECODE = -11,         // 值不能反序列化为指定 rpcdata
	OI_ERR_ENCODE = -12,
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
