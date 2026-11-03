#pragma once

#include "oi_bytes_stream.h"
#include "oi_session_manager.h"
#include <functional>
namespace OI { namespace Rpc {

// 分片逐个取快照，用于诊断；有并发请求时不保证全局同一时刻的一致性。
struct RpcWaitStats { size_t calls = 0; size_t expirations = 0; size_t buckets = 0; };
RpcWaitStats GetRpcWaitStats();

bool MarkRpcAwaitingConnect(uint64_t call_id);
bool ClaimRpcSend(uint64_t call_id, sid_t sid);
// 回包按调用号交给 RpcCall 登记的等待项，请求交给该协议号的处理协程。都没有就把包丢掉。
bool DeliverRpc(proto_type_t id, ::OI::Base::BytesStream& body, ::OI::Net::SessionManager* mgr, sid_t sid);

}}
