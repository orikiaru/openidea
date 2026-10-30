#ifndef __jxy_rpc_h__
#define __jxy_rpc_h__

#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include <functional>
namespace OI { namespace Rpc {

// 协议号 -> 这次调用传进来的 res。回包到了就调用它自己的 deserialize。
void WatchRpc(proto_type_t id, std::function<void(::OI::Base::BytesStream&)> decode);
void MarkRpcAwaitingConnect(uint64_t call_id);
bool ClaimRpcSend(uint64_t call_id, sid_t sid);
// 先交给 RpcCall 登记的 res，否则交给该协议号的协程。都没有就把包丢掉。
bool DeliverRpc(proto_type_t id, ::OI::Base::BytesStream& body, ::OI::Net::SessionManager* mgr, sid_t sid);

}}
#endif
