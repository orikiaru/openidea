#pragma once

#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include <functional>
namespace OI { namespace Rpc {

void MarkRpcAwaitingConnect(uint64_t call_id);
bool ClaimRpcSend(uint64_t call_id, sid_t sid);
// 回包按调用号交给 RpcCall 登记的等待项，请求交给该协议号的处理协程。都没有就把包丢掉。
bool DeliverRpc(proto_type_t id, ::OI::Base::BytesStream& body, ::OI::Net::SessionManager* mgr, sid_t sid);

}}
