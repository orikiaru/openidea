#include "rpc.h"
#include "base_timer_manager.h"
#include "proto_stub_manager.h"

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
	os << _rid;
	if(IsReq())
		serialize_req(os);
	else
		serialize_res(os);
	return os;
}

BytesStream& Rpc::deserialize(BytesStream& os)
{
	os >> _rid;
	if(IsReq())
	{
		return deserialize_req(os);
	}
	else
	{
		Rpc* pRpc = RpcKeeper::GetInstance().FindAndRemoveStub(_rid);
		if(!pRpc)
		{
			//TODO LOG
			//没有存根 流化给自己
			return deserialize_res(os);
		}
		//流化存根
		pRpc->deserialize_res(os);
		SetResStub(pRpc);
	}
	return os;
}

