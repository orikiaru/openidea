#include "rpc.h"
#include "base_timer_manager.h"
#include "poll.h"
#include "proto_stub_manager.h"
#include "rpc_coro.h"
#include "threadpool.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <list>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>
#include <vector>

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
		OI::Thread::ThreadPool::GetInstance().SetDrain(DrainStCoro);
		OI::Thread::ThreadPool::GetInstance().SetPending(PendingStCoro);
		OI::Thread::ThreadPool::GetInstance().SetAfterResume(coro::DrainDetached);
	}
} g_st_drain_reg;

rpc_id_t RpcKeeper::GetRpcID()
{
	OI::Thread::SpinLockGuard l(_rid_locker);
	//assert(s_rpc_id < RPC_REQ_MASK);
	return s_rpc_id++ | RPC_REQ_MASK;
}

void RpcKeeper::AddStub(Rpc* pRpc)
{
	rpc_id_t rid = (pRpc->GetRID() & ~RPC_REQ_MASK);
	assert(pRpc && !FindStub(rid));
	OI::Thread::MutexGuard l(_locker);
	s_rpc_map[rid] = pRpc;
	BaseTimer* pTimer = new RpcTimeoutTimer(rid);
	s_rpc_timer_map[rid] = pTimer;
	BaseTimerManager::GetInstance().AddTimer(pTimer, pRpc->TimeOut() * 1000);
}

Rpc* RpcKeeper::FindStub(rpc_id_t rid)
{
	OI::Thread::MutexGuard l(_locker);
	RpcMap::iterator it = s_rpc_map.find(rid);	
	return it != s_rpc_map.end() ? it->second: NULL;
}

Rpc* RpcKeeper::FindAndRemoveStub(rpc_id_t rid)
{
	OI::Thread::MutexGuard l(_locker);
	RpcMap::iterator it = s_rpc_map.find(rid);	
	Rpc* pRPC = NULL;
	if(it != s_rpc_map.end())
	{
		pRPC = it->second;
		s_rpc_map.erase(it);
	}
	RpcTimerMap::iterator tit = s_rpc_timer_map.find(rid);
	if (tit != s_rpc_timer_map.end())
	{
		//如果正在触发,则等Timer自行销毁
		//这里取巧了,因为Timer触发会走到这里,锁互斥,内存无法回收销毁
		//所以对于Timer来说,要么从这删除,要么已经移除timer队列,其他情况的定时器不推荐这么写
		if (BaseTimerManager::GetInstance().RemoveTimer(tit->second))
		{
			SAFE_DELETE(tit->second);
		}
		s_rpc_timer_map.erase(tit);
	}
	return pRPC;
}

void RpcKeeper::DestoryStub(rpc_id_t rid)
{
	Rpc* pRpc = NULL;
	{
		OI::Thread::MutexGuard l(_locker);
		RpcMap::iterator it = s_rpc_map.find(rid);	
		if(it != s_rpc_map.end())
		{
			pRpc = it->second;
		}
		s_rpc_map.erase(it);
	}
	assert(pRpc);
	SAFE_DELETE(pRpc);
}

Rpc* RpcKeeper::CreateRpc(proto_type_t type)
{
	Rpc* pRpc = dynamic_cast<Rpc*>(ProtocolStubs::GetInstance().GetStub(type)->Clone());
	pRpc->SetRID(RpcKeeper::GetRpcID());
	RpcKeeper::AddStub(pRpc);
	return pRpc;
}

Rpc::Rpc(const Rpc& that):
	TimerProtocol(that),
	_rid(that._rid),
	_req(that._req->Clone()),
	_res(that._res->Clone()) 
{}

Rpc* Rpc::Call(proto_type_t id, OI::Rpc::RpcData* p_data)
{
	Rpc* pRpc = RpcKeeper::GetInstance().CreateRpc(id);
	if (!pRpc)
	{
		return NULL;
	}
	pRpc->SetReq(p_data);
	return pRpc;
}


void Rpc::Handle(SessionManager* mgr, sid_t sid)
{
	if(IsReq())
	{
		DoRequest(mgr, sid);
		ClearReq();
		mgr->Send(sid, this);
	}
	else
	{
		if (_p_stub && _p_stub != this) //==this情况,当前只可能是rpc超时
		{
			_p_stub->DoResponse(mgr, sid);
			SAFE_DELETE(_p_stub);
		}
		else
		{
			//TODO 可能需要比对数据
		}
	}
}

OI::Base::BytesStream& Rpc::serialize(OI::Base::BytesStream& os) const
{
	if (IsReq())
	{
		return serialize_req(os);
	}
	return serialize_res(os);
}

OI::Base::BytesStream& Rpc::deserialize(OI::Base::BytesStream& os)
{
	if (IsReq())
	{
		return deserialize_req(os);
	}
	return deserialize_res(os);
}

namespace
{
typedef std::function<coro::Task(coro::Token, SessionManager*, sid_t, OI::Base::BytesStream)> RpcHandleFn;

struct RpcCoroSlot
{
	std::coroutine_handle<> handle;
	int* result;
	std::function<bool(OI::Base::BytesStream&)> decode;
	std::function<void()> resume;
	proto_type_t res_id;
};

struct RpcExpire
{
	int64_t deadline_ms;
	uint64_t call_id;
};

// 等回包的调用按调用号分片，发请求的工人之间不抢同一把锁。
// 超时按时长分桶，同一桶里先登记的先到期，扫的时候只看队头。
struct RpcCallShard
{
	OI::Thread::Mutex mu;
	std::unordered_map<uint64_t, RpcCoroSlot> slots;
	std::map<int, std::deque<RpcExpire> > expire;
};

const uint64_t kCallShards = 64;
const int kSweepMs = 200;
RpcCallShard g_call_shards[kCallShards];
std::atomic<uint64_t> g_call_seq(1);
std::once_flag g_sweep_once;

OI::Thread::Mutex g_rpc_wait_lock;
std::map<proto_type_t, std::deque<std::function<void(OI::Base::BytesStream&)> > > g_rpc_wait;
std::map<proto_type_t, RpcHandleFn> g_rpc_handle;
std::set<proto_type_t> g_rpc_res;

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

RpcCallShard& ShardOf(uint64_t call_id)
{
	return g_call_shards[call_id % kCallShards];
}

void FinishRpcSlot(RpcCoroSlot& slot, int code, OI::Base::BytesStream* body)
{
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

bool TakeRpcSlot(uint64_t call_id, RpcCoroSlot& out)
{
	RpcCallShard& shard = ShardOf(call_id);
	OI::Thread::MutexGuard lock(shard.mu);
	std::unordered_map<uint64_t, RpcCoroSlot>::iterator it = shard.slots.find(call_id);
	if (it == shard.slots.end())
	{
		return false;
	}
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
		OI::Thread::MutexGuard lock(shard.mu);
		for (std::map<int, std::deque<RpcExpire> >::iterator bit = shard.expire.begin(); bit != shard.expire.end(); ++bit)
		{
			std::deque<RpcExpire>& q = bit->second;
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
				fired.push_back(std::move(it->second));
				shard.slots.erase(it);
				q.pop_front();
			}
		}
	}
	for (size_t i = 0; i < fired.size(); ++i)
	{
		FinishRpcSlot(fired[i], -2, NULL);
	}
}

class RpcSweepTimer : public BaseTimer
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
	BaseTimerManager::GetInstance().AddTimer(new RpcSweepTimer(), kSweepMs);
}
}

void RegisterRpcRes(proto_type_t res_id)
{
	OI::Thread::MutexGuard lock(g_rpc_wait_lock);
	g_rpc_res.insert(res_id);
}

uint64_t BindRpcCall(proto_type_t res_id, std::coroutine_handle<> handle, int* result, int timeout_sec, std::function<bool(OI::Base::BytesStream&)> decode, std::function<void()> resume)
{
	std::call_once(g_sweep_once, []() {
		Poll::GetInstance().CallOnLoop(ArmRpcSweep, NULL);
	});
	uint64_t call_id = g_call_seq.fetch_add(1, std::memory_order_relaxed);
	RpcCoroSlot slot;
	slot.handle = handle;
	slot.result = result;
	slot.decode = std::move(decode);
	slot.resume = std::move(resume);
	slot.res_id = res_id;
	RpcCallShard& shard = ShardOf(call_id);
	OI::Thread::MutexGuard lock(shard.mu);
	shard.slots.emplace(call_id, std::move(slot));
	if (timeout_sec > 0)
	{
		RpcExpire e;
		e.deadline_ms = NowMs() + (int64_t)timeout_sec * 1000;
		e.call_id = call_id;
		shard.expire[timeout_sec].push_back(e);
	}
	return call_id;
}

bool WakeRpcCall(uint64_t call_id, int code, OI::Base::BytesStream* body)
{
	RpcCoroSlot slot;
	if (!TakeRpcSlot(call_id, slot))
	{
		return false;
	}
	FinishRpcSlot(slot, code, body);
	return true;
}

void WatchRpc(proto_type_t id, std::function<void(OI::Base::BytesStream&)> decode)
{
	OI::Thread::MutexGuard lock(g_rpc_wait_lock);
	g_rpc_wait[id].push_back(std::move(decode));
}

void BindRpcHandler(proto_type_t id, RpcHandleFn handle)
{
	OI::Thread::MutexGuard lock(g_rpc_wait_lock);
	g_rpc_handle[id] = std::move(handle);
}

bool DeliverRpc(proto_type_t id, OI::Base::BytesStream& body, SessionManager* mgr, sid_t sid)
{
	std::function<void(OI::Base::BytesStream&)> decode;
	const RpcHandleFn* handle = NULL;
	bool is_res = false;
	{
		OI::Thread::MutexGuard lock(g_rpc_wait_lock);
		if (g_rpc_res.find(id) != g_rpc_res.end())
		{
			is_res = true;
		}
		else
		{
			std::map<proto_type_t, std::deque<std::function<void(OI::Base::BytesStream&)> > >::iterator it = g_rpc_wait.find(id);
			if (it != g_rpc_wait.end() && !it->second.empty())
			{
				decode = std::move(it->second.front());
				it->second.pop_front();
				if (it->second.empty())
				{
					g_rpc_wait.erase(it);
				}
			}
			else
			{
				std::map<proto_type_t, RpcHandleFn>::iterator hit = g_rpc_handle.find(id);
				if (hit == g_rpc_handle.end())
				{
					return false;
				}
				// 处理函数只在启动时注册，map 节点不会再动，出锁后用指针调用。
				handle = &hit->second;
			}
		}
	}
	try
	{
		if (is_res)
		{
			uint64_t call_id = 0;
			body >> call_id;
			RpcCoroSlot slot;
			if (!TakeRpcSlot(call_id, slot))
			{
				return true;
			}
			if (slot.res_id != id)
			{
				FinishRpcSlot(slot, -1, NULL);
				return true;
			}
			FinishRpcSlot(slot, 0, &body);
		}
		else if (decode)
		{
			decode(body);
		}
		else
		{
			coro::StartTask([handle, mgr, sid, &body](coro::Token token) {
				return (*handle)(token, mgr, sid, std::move(body));
			});
		}
	}
	catch (OI::Base::ParserException&)
	{
	}
	return true;
}
