#pragma once
#include <stdint.h>

namespace OI { namespace Base {

// 定制协议的号。生成出来的 rpc 号只做 req/res 映射，和这里撞了也不是同一条协议。
enum ProtoId
{
	PROTO_LOG_REMOTE = 1,
	PROTO_LOG_CLIENT_REGISTER = 2,
	PROTO_REDIS = 3,
	PROTO_REG_REGISTER = 4,
	PROTO_REG_REGISTER_RES = 5,
	PROTO_REG_HEARTBEAT = 6,
	PROTO_REG_HEARTBEAT_RES = 7,
	PROTO_REG_LOOKUP = 8,
	PROTO_REG_LOOKUP_RES = 9,
};

}}
