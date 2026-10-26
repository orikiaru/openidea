#include "rpc.h"
#include "base_timer_manager.h"
#include "proto_stub_manager.h"
#include "rpc_coro.h"
#include "threadpool.h"
#include <deque>
#include <map>
#include <mutex>

static bool DrainStCoro()
{
	return coro::StScheduler::Poll();
}

static struct StDrainReg
{
	StDrainReg()
	{
		ThreadPool::GetInstance()->SetDrain(DrainStCoro);
	}
} g_st_drain_reg;

rpc_id_t RpcKeeper::GetRpcID()
{
	SpinLockGuard l(_rid_locker);
	//assert(s_rpc_id < RPC_REQ_MASK);
	return s_rpc_id++ | RPC_REQ_MASK;
}

void RpcKeeper::AddStub(Rpc* pRpc)
{
	rpc_id_t rid = (pRpc->GetRID() & ~RPC_REQ_MASK);
	assert(pRpc && !FindStub(rid));
	MutexGuard l(_locker);
	s_rpc_map[rid] = pRpc;
	BaseTimer* pTimer = new RpcTimeoutTimer(rid);
	s_rpc_timer_map[rid] = pTimer;
	BaseTimerManager::GetInstance()->AddTimer(pTimer, pRpc->TimeOut() * 1000);
}

Rpc* RpcKeeper::FindStub(rpc_id_t rid)
{
	MutexGuard l(_locker);
	RpcMap::iterator it = s_rpc_map.find(rid);	
	return it != s_rpc_map.end() ? it->second: NULL;
}

Rpc* RpcKeeper::FindAndRemoveStub(rpc_id_t rid)
{
	MutexGuard l(_locker);
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
		if (BaseTimerManager::GetInstance()->RemoveTimer(tit->second))
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
		MutexGuard l(_locker);
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

Rpc* Rpc::Call(proto_type_t id, RpcData* p_data)
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

BytesStream& Rpc::serialize(BytesStream& os) const
{
	if (IsReq())
	{
		return serialize_req(os);
	}
	return serialize_res(os);
}

BytesStream& Rpc::deserialize(BytesStream& os)
{
	if (IsReq())
	{
		return deserialize_req(os);
	}
	return deserialize_res(os);
}

namespace
{
struct RpcCoroSlot
{
	std::coroutine_handle<> handle;
	int* result;
	std::function<bool(BytesStream&)> decode;
	std::function<void()> resume;
	BaseTimer* timer;
	proto_type_t res_id;
};

std::mutex g_rpc_wait_lock;
std::map<proto_type_t, std::deque<std::function<void(BytesStream&)> > > g_rpc_wait;
std::map<proto_type_t, std::function<void(SessionManager*, sid_t, BytesStream&)> > g_rpc_handle;
std::map<uint64_t, RpcCoroSlot> g_rpc_coro;
std::map<proto_type_t, std::deque<uint64_t> > g_rpc_coro_q;
uint64_t g_rpc_coro_seq = 1;

class RpcCoroTimer : public BaseTimer
{
	uint64_t _rid;
public:
	explicit RpcCoroTimer(uint64_t rid) : _rid(rid) {}
	bool OnTimer()
	{
		WakeRpcCall(_rid, -2, NULL);
		RpcCoroTimer* self = this;
		SAFE_DELETE(self);
		return false;
	}
};
}

uint64_t BindRpcCall(proto_type_t res_id, std::coroutine_handle<> handle, int* result, int timeout_sec, std::function<bool(BytesStream&)> decode, std::function<void()> resume)
{
	std::lock_guard<std::mutex> lock(g_rpc_wait_lock);
	uint64_t rid = g_rpc_coro_seq++;
	if (g_rpc_coro_seq == 0)
	{
		g_rpc_coro_seq = 1;
	}
	RpcCoroTimer* timer = NULL;
	if (timeout_sec > 0)
	{
		timer = new RpcCoroTimer(rid);
		BaseTimerManager::GetInstance()->AddTimer(timer, timeout_sec * 1000);
	}
	RpcCoroSlot slot;
	slot.handle = handle;
	slot.result = result;
	slot.decode = std::move(decode);
	slot.resume = std::move(resume);
	slot.timer = timer;
	slot.res_id = res_id;
	g_rpc_coro[rid] = std::move(slot);
	g_rpc_coro_q[res_id].push_back(rid);
	return rid;
}

bool WakeRpcCall(uint64_t rid, int code, BytesStream* body)
{
	std::coroutine_handle<> handle;
	int* result = NULL;
	std::function<bool(BytesStream&)> decode;
	std::function<void()> resume;
	BaseTimer* timer = NULL;
	{
		std::lock_guard<std::mutex> lock(g_rpc_wait_lock);
		std::map<uint64_t, RpcCoroSlot>::iterator it = g_rpc_coro.find(rid);
		if (it == g_rpc_coro.end())
		{
			return false;
		}
		handle = it->second.handle;
		result = it->second.result;
		decode = std::move(it->second.decode);
		resume = std::move(it->second.resume);
		timer = it->second.timer;
		if (!body)
		{
			std::map<proto_type_t, std::deque<uint64_t> >::iterator qit = g_rpc_coro_q.find(it->second.res_id);
			if (qit != g_rpc_coro_q.end())
			{
				for (std::deque<uint64_t>::iterator dit = qit->second.begin(); dit != qit->second.end(); ++dit)
				{
					if (*dit == rid)
					{
						qit->second.erase(dit);
						break;
					}
				}
				if (qit->second.empty())
				{
					g_rpc_coro_q.erase(qit);
				}
			}
		}
		g_rpc_coro.erase(it);
	}
	if (body)
	{
		if (decode)
		{
			decode(*body);
		}
		if (timer && BaseTimerManager::GetInstance()->RemoveTimer(timer))
		{
			SAFE_DELETE(timer);
		}
	}
	else if (result)
	{
		*result = code;
	}
	if (resume)
	{
		resume();
	}
	else if (handle)
	{
		coro::StScheduler::Post(handle);
	}
	return true;
}

void WatchRpc(proto_type_t id, std::function<void(BytesStream&)> decode)
{
	std::lock_guard<std::mutex> lock(g_rpc_wait_lock);
	g_rpc_wait[id].push_back(std::move(decode));
}

void HandleRpc(proto_type_t id, std::function<void(SessionManager*, sid_t, BytesStream&)> handle)
{
	std::lock_guard<std::mutex> lock(g_rpc_wait_lock);
	g_rpc_handle[id] = std::move(handle);
}

bool DeliverRpc(proto_type_t id, BytesStream& body, SessionManager* mgr, sid_t sid)
{
	std::function<void(BytesStream&)> decode;
	std::function<void(SessionManager*, sid_t, BytesStream&)> handle;
	uint64_t rid = 0;
	bool have_rid = false;
	{
		std::lock_guard<std::mutex> lock(g_rpc_wait_lock);
		std::map<proto_type_t, std::deque<uint64_t> >::iterator qit = g_rpc_coro_q.find(id);
		if (qit != g_rpc_coro_q.end() && !qit->second.empty())
		{
			rid = qit->second.front();
			qit->second.pop_front();
			if (qit->second.empty())
			{
				g_rpc_coro_q.erase(qit);
			}
			have_rid = true;
		}
		else
		{
		std::map<proto_type_t, std::deque<std::function<void(BytesStream&)> > >::iterator it = g_rpc_wait.find(id);
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
			std::map<proto_type_t, std::function<void(SessionManager*, sid_t, BytesStream&)> >::iterator hit = g_rpc_handle.find(id);
			if (hit == g_rpc_handle.end())
			{
				return false;
			}
			handle = hit->second;
		}
		}
	}
	if (have_rid)
	{
		WakeRpcCall(rid, 0, &body);
		return true;
	}
	try
	{
		if (decode)
		{
			decode(body);
		}
		else
		{
			handle(mgr, sid, body);
		}
	}
	catch (ParserException&)
	{
	}
	return true;
}

namespace coro
{
class CoroRpcTimer : public BaseTimer
{
	uint64_t _rid;
public:
	explicit CoroRpcTimer(uint64_t rid) : _rid(rid) {}
	bool OnTimer()
	{
		Hub::CompleteRaw(_rid, -1, NULL);
		CoroRpcTimer* self = this;
		SAFE_DELETE(self);
		return false;
	}
};

void ArmRpcTimeout(uint64_t rid, int seconds)
{
	if (seconds <= 0)
	{
		return;
	}
	BaseTimerManager::GetInstance()->AddTimer(new CoroRpcTimer(rid), seconds * 1000);
}
}

