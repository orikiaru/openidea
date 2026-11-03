#include "oi_rpc_call.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_rpc_coro.h"
#include "oi_thread.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>
namespace OI { namespace Rpc {

static bool DrainStCoro()
{
	return coro::StScheduler::Poll();
}

static bool PendingStCoro()
{
	return coro::StScheduler::Queued();
}

static struct StDrainReg
{
	StDrainReg()
	{
		::OI::Thread::ThreadPool::GetInstance().SetDrain(DrainStCoro);
		::OI::Thread::ThreadPool::GetInstance().SetPending(PendingStCoro);
		::OI::Thread::ThreadPool::GetInstance().SetAfterResume(coro::DrainDetached);
	}
} g_st_drain_reg;

namespace
{
typedef std::function<coro::Task(coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> RpcHandleFn;

struct RpcExpire
{
	int64_t deadline_ms;
	uint64_t call_id;
};
using RpcExpireBuckets = std::map<int64_t, std::list<RpcExpire>>;

struct RpcCoroSlot
{
	::OI::Net::SessionManager* mgr;
	sid_t sid;
	std::coroutine_handle<> handle;
	int32_t* result;
	std::function<bool(::OI::Base::BytesStream&)> decode;
	std::function<void()> resume;
	proto_type_t res_id;
	uint64_t call_id;
	bool awaiting_connect;
	bool has_expiry = false;
	RpcExpireBuckets::iterator expiry_bucket;
	std::list<RpcExpire>::iterator expiry;
};

// 同时长按登记顺序排队；槽直接持有索引，请求结束时 O(1) 摘除对应节点。
struct RpcCallShard
{
	::OI::Thread::Mutex mu;
	std::unordered_map<uint64_t, RpcCoroSlot> slots;
	RpcExpireBuckets expire;
};

// 调用者持分片锁。删除空桶，避免动态超时参数让桶本身无限增长。
void RemoveRpcExpiry(RpcCallShard& shard, RpcCoroSlot& slot)
{
	if (!slot.has_expiry) return;
	auto bucket = slot.expiry_bucket;
	bucket->second.erase(slot.expiry);
	if (bucket->second.empty()) shard.expire.erase(bucket);
	slot.has_expiry = false;
	slot.expiry = {};
	slot.expiry_bucket = {};
}

const uint64_t kCallShards = 64;
const int kSweepMs = 200;
RpcCallShard g_call_shards[kCallShards];
std::atomic<uint64_t> g_call_seq(1);
std::once_flag g_sweep_once;

::OI::Thread::Mutex g_rpc_handle_lock;
std::map<proto_type_t, RpcHandleFn> g_rpc_handle;

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

RpcCallShard& ShardOf(uint64_t call_id)
{
	return g_call_shards[call_id % kCallShards];
}

void FinishRpcSlot(RpcCoroSlot& slot, int32_t code, ::OI::Base::BytesStream* body)
{
	if (slot.awaiting_connect) slot.mgr->DropPendingCall(slot.call_id);
	if (body)
	{
		if (slot.decode)
		{
			slot.decode(*body);
		}
	}
	else if (slot.result)
	{
		*slot.result = code;
	}
	if (slot.resume)
	{
		slot.resume();
	}
	else if (slot.handle)
	{
		coro::Schedule(slot.handle);
	}
}

bool TakeRpcSlot(uint64_t call_id, RpcCoroSlot& out, ::OI::Net::SessionManager* mgr = NULL, sid_t sid = 0, proto_type_t res_id = 0)
{
	RpcCallShard& shard = ShardOf(call_id);
	::OI::Thread::MutexGuard lock(shard.mu);
	std::unordered_map<uint64_t, RpcCoroSlot>::iterator it = shard.slots.find(call_id);
	if (it == shard.slots.end())
	{
		return false;
	}
	// 来源或协议错误不能消费其他连接的等待项。
	if (mgr && (it->second.mgr != mgr || it->second.sid != sid || it->second.res_id != res_id)) return false;
	RemoveRpcExpiry(shard, it->second);
	out = std::move(it->second);
	shard.slots.erase(it);
	return true;
}

void SweepRpcCalls()
{
	int64_t now = NowMs();
	std::vector<RpcCoroSlot> fired;
	for (uint64_t i = 0; i < kCallShards; ++i)
	{
		RpcCallShard& shard = g_call_shards[i];
		::OI::Thread::MutexGuard lock(shard.mu);
		for (auto bit = shard.expire.begin(); bit != shard.expire.end(); )
		{
			std::list<RpcExpire>& q = bit->second;
			while (!q.empty())
			{
				std::unordered_map<uint64_t, RpcCoroSlot>::iterator it = shard.slots.find(q.front().call_id);
				if (it == shard.slots.end())
				{
					q.pop_front();
					continue;
				}
				if (q.front().deadline_ms > now)
				{
					break;
				}
				it->second.has_expiry = false; // 本轮在桶内直接摘除，避免使外层迭代器失效。
				it->second.expiry = {};
				it->second.expiry_bucket = {};
				fired.push_back(std::move(it->second));
				shard.slots.erase(it);
				q.pop_front();
			}
			if (q.empty()) bit = shard.expire.erase(bit);
			else ++bit;
		}
	}
	for (size_t i = 0; i < fired.size(); ++i)
	{
		// 还没连上就到期，按连接失败返回，不占超时码。
		int32_t code = fired[i].awaiting_connect ? OI_ERR_CONNECT : OI_ERR_TIMEOUT;
		FinishRpcSlot(fired[i], code, NULL);
	}
}

class RpcSweepTimer : public ::OI::Timer::BaseTimer
{
public:
	bool OnTimer()
	{
		SweepRpcCalls();
		return true;
	}
};

void ArmRpcSweep(void*)
{
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new RpcSweepTimer(), kSweepMs);
}
}

RpcWaitStats GetRpcWaitStats()
{
	RpcWaitStats stats{};
	for (auto& shard : g_call_shards)
	{
		::OI::Thread::MutexGuard lock(shard.mu);
		stats.calls += shard.slots.size();
		stats.buckets += shard.expire.size();
		for (const auto& bucket : shard.expire) stats.expirations += bucket.second.size();
	}
	return stats;
}

bool MarkRpcAwaitingConnect(uint64_t call_id)
{
	RpcCallShard& shard = ShardOf(call_id);
	::OI::Thread::MutexGuard lock(shard.mu);
	std::unordered_map<uint64_t, RpcCoroSlot>::iterator it = shard.slots.find(call_id);
	if (it == shard.slots.end())
	{
		return false;
	}
	it->second.awaiting_connect = true;
	return true;
}

bool ClaimRpcSend(uint64_t call_id, sid_t sid)
{
	RpcCallShard& shard = ShardOf(call_id);
	::OI::Thread::MutexGuard lock(shard.mu);
	std::unordered_map<uint64_t, RpcCoroSlot>::iterator it = shard.slots.find(call_id);
	if (it == shard.slots.end())
	{
		return false;
	}
	it->second.awaiting_connect = false;
	it->second.sid = sid;
	return true;
}

uint64_t BindRpcCall(::OI::Net::SessionManager* mgr, sid_t sid, proto_type_t res_id, std::coroutine_handle<> handle, int32_t* result, int64_t timeout_ms, std::function<bool(::OI::Base::BytesStream&)> decode, std::function<void()> resume)
{
	std::call_once(g_sweep_once, []() {
		::OI::Net::Poll::GetInstance().CallOnLoop(ArmRpcSweep, NULL);
	});
	// 最高位留给请求标记，槽里只存去掉标记的号。
	uint64_t call_id = g_call_seq.fetch_add(1, std::memory_order_relaxed) & ~(uint64_t)RPC_REQ_MASK;
	RpcCoroSlot slot;
	slot.mgr = mgr;
	slot.sid = sid;
	slot.handle = handle;
	slot.result = result;
	slot.decode = std::move(decode);
	slot.resume = std::move(resume);
	slot.res_id = res_id;
	slot.call_id = call_id;
	slot.awaiting_connect = false;
	RpcCallShard& shard = ShardOf(call_id);
	::OI::Thread::MutexGuard lock(shard.mu);
	auto inserted = shard.slots.emplace(call_id, std::move(slot)).first;
	try
	{
		if (timeout_ms > 0)
		{
			auto bucket = shard.expire.try_emplace(timeout_ms).first;
			try
			{
				const int64_t now = NowMs();
				const int64_t max_time = (std::numeric_limits<int64_t>::max)();
				bucket->second.push_back({timeout_ms > max_time - now ? max_time : now + timeout_ms, call_id});
			}
			catch (...)
			{
				if (bucket->second.empty()) shard.expire.erase(bucket);
				throw;
			}
			inserted->second.expiry_bucket = bucket;
			inserted->second.expiry = std::prev(bucket->second.end());
			inserted->second.has_expiry = true;
		}
	}
	catch (...)
	{
		// 登记失败必须在放锁前撤销：调用者还没有拿到调用号，不能自行清理。
		shard.slots.erase(inserted);
		throw;
	}
	return call_id;
}

bool WakeRpcCall(uint64_t call_id, int32_t code, ::OI::Base::BytesStream* body)
{
	RpcCoroSlot slot;
	if (!TakeRpcSlot(call_id, slot))
	{
		return false;
	}
	FinishRpcSlot(slot, code, body);
	return true;
}

void FailRpcCalls(::OI::Net::SessionManager* mgr, sid_t sid)
{
	std::vector<RpcCoroSlot> failed;
	for (auto& shard : g_call_shards)
	{
		::OI::Thread::MutexGuard lock(shard.mu);
		for (auto it = shard.slots.begin(); it != shard.slots.end(); )
		{
			if (it->second.mgr == mgr && it->second.sid == sid)
			{
				RemoveRpcExpiry(shard, it->second);
				failed.push_back(std::move(it->second));
				it = shard.slots.erase(it);
			}
			else ++it;
		}
	}
	// 唤醒放在锁外，应用可以立即发起新的调用。
	for (auto& slot : failed) FinishRpcSlot(slot, OI_ERR_CONNECT, NULL);
}

void BindRpcHandler(proto_type_t id, RpcHandleFn handle)
{
	::OI::Thread::MutexGuard lock(g_rpc_handle_lock);
	g_rpc_handle[id] = std::move(handle);
}

bool DeliverRpc(proto_type_t id, ::OI::Base::BytesStream& body, ::OI::Net::SessionManager* mgr, sid_t sid)
{
	uint64_t raw = 0;
	bool is_req = false;
	try
	{
		body.StartTransaction();
		body >> raw;
		body.RollBack();
		is_req = (raw & (uint64_t)RPC_REQ_MASK) != 0;
	}
	catch (::OI::Base::ParserException&)
	{
		return false;
	}

	const RpcHandleFn* handle = NULL;
	if (is_req)
	{
		::OI::Thread::MutexGuard lock(g_rpc_handle_lock);
		std::map<proto_type_t, RpcHandleFn>::iterator hit = g_rpc_handle.find(id);
		if (hit == g_rpc_handle.end())
		{
			return false;
		}
		// 处理函数只在启动时注册，map 节点不会再动，出锁后用指针调用。
		handle = &hit->second;
	}
	try
	{
		if (!is_req)
		{
			uint64_t call_id = 0;
			body >> call_id;
			call_id &= ~(uint64_t)RPC_REQ_MASK;
			RpcCoroSlot slot;
			if (!TakeRpcSlot(call_id, slot, mgr, sid, id))
			{
				return true;
			}
			FinishRpcSlot(slot, 0, &body);
		}
		else
		{
			coro::StartTask([handle, mgr, sid, &body](coro::Token token) {
				return (*handle)(token, mgr, sid, std::move(body));
			});
		}
	}
	catch (::OI::Base::ParserException&)
	{
	}
	return true;
}
}}
