#pragma once
#include "framework_protocol/rpc_ids.h"

namespace OI
{
namespace Base
{

// 框架协议号。定义在 cbaselib/proto/protocol/framework.proto，从 -1000 起。
using ::OI::RpcData::PROTO_LOG_REMOTE;
using ::OI::RpcData::PROTO_LOG_CLIENT_REGISTER;
using ::OI::RpcData::PROTO_REDIS;
using ::OI::RpcData::PROTO_REG_REGISTER;
using ::OI::RpcData::PROTO_REG_HEARTBEAT;
using ::OI::RpcData::PROTO_REG_PULL;

}
}
