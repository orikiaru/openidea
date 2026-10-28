#ifndef __jxy_rpc_h__
#define __jxy_rpc_h__

#include "oi_protocol.h"
#include "oi_timer.h"
#include "oi_singleton.h"
#include <functional>

//定时器加入后使用
class TimerProtocol: public Protocol
{
public:
	explicit TimerProtocol():Protocol(){}
	TimerProtocol(const TimerProtocol& that) :Protocol(that) {}
	TimerProtocol(proto_type_t type):Protocol(type){}
	virtual int  TimeOut() = 0;		//获取超时时间
	virtual void OnTimeOut() = 0;	//超时处理
};

static bool IsReqID(rpc_id_t rid) 		{ return rid & RPC_REQ_MASK; }
static void ClearReq(rpc_id_t& rid) 	{ rid &= ~RPC_REQ_MASK;}

class Rpc : public TimerProtocol
{
public:
	enum
	{
		RPC_PB_RID = 0,
		RPC_PB_REQ = 1,
		RPC_PB_RES = 2,
	};
protected:
	rpc_id_t _rid;
	OI::Rpc::RpcData* _req;
	OI::Rpc::RpcData* _res;
	Rpc*	 _p_stub;	//rpc回执时的临时存根保存,不再去RpcKeeper重复获取了
public:
	Rpc(proto_type_t type, OI::Rpc::RpcData* req, OI::Rpc::RpcData* res) :TimerProtocol(type), _rid(0), _req(req), _res(res), _p_stub(NULL){}
	Rpc(const Rpc& that);


	virtual ~Rpc()
	{
		SAFE_DELETE(_req);
		SAFE_DELETE(_res);
	}

	void SetRID(rpc_id_t rid) { _rid = rid; }
	rpc_id_t GetRID()const { return _rid; }
	bool IsReq()const { return IsReqID(_rid); }
	void ClearReq() { ::ClearReq(_rid); }


	virtual void Handle(SessionManager* mgr, sid_t sid);

	virtual OI::Base::BytesStream& serialize(OI::Base::BytesStream& os) const;
	virtual OI::Base::BytesStream& deserialize(OI::Base::BytesStream& os);
	
	//设置req的纯虚函数,子类检查reqtype是否匹配
	virtual void SetReq(OI::Rpc::RpcData* req)
	{
		assert(false);	//需要检查Req类型是否匹配,不走基类,基类给个示例
		SAFE_DELETE(_req);
		_req = req;
	}

	//以下是模块自定义协议处理
	virtual void DoRequest(SessionManager* mgr, sid_t sid) = 0;
	virtual void DoResponse(SessionManager* mgr, sid_t sid) = 0;
	//实现的RPC实例通过重写req res序列化与反序列化实现具体rpc内容
	virtual OI::Base::BytesStream& serialize_req(OI::Base::BytesStream& os) const	{ return os << *_req; }
	virtual OI::Base::BytesStream& deserialize_req(OI::Base::BytesStream& os)		{ return os >> *_req; }
	virtual OI::Base::BytesStream& serialize_res(OI::Base::BytesStream& os) const	{ return os << *_res; }
	virtual OI::Base::BytesStream& deserialize_res(OI::Base::BytesStream& os)		{ return os >> *_res; }

private:
	void SetResStub(Rpc* p_rpc) { _p_stub = p_rpc;  }
};

// 协议号 -> 这次调用传进来的 res。回包到了就调用它自己的 deserialize。
void WatchRpc(proto_type_t id, std::function<void(OI::Base::BytesStream&)> decode);
// 先交给 RpcCall 登记的 res，否则交给该协议号的协程。都没有就把包丢掉。
bool DeliverRpc(proto_type_t id, OI::Base::BytesStream& body, SessionManager* mgr, sid_t sid);
	

//可以考虑绑定到回话管理器上,一个会话管理器保存自身所有RPC存根,减少锁冲突
class RpcKeeper
{
	DECLARE_SINGLETON(RpcKeeper)
	class RpcTimeoutTimer : public BaseTimer
	{
		friend class RpcKeeper;
		rpc_id_t _rid;
		RpcTimeoutTimer(rpc_id_t rid) :_rid(rid) {}
		virtual bool OnTimer()
		{
			Rpc* pRpc = RpcKeeper::GetInstance().FindAndRemoveStub(_rid);
			if (!pRpc)
			{
				return false;
			}
			pRpc->OnTimeOut();
			SAFE_DELETE(pRpc);
			RpcTimeoutTimer* pTimer = this;
			SAFE_DELETE(pTimer);	//触发完干掉自己,映射关系在FindAndRemoveStub已经解除
			return false;
		}
	};

	typedef std::map<rpc_id_t, Rpc*> RpcMap;
	typedef std::map<rpc_id_t, BaseTimer*> RpcTimerMap;

	RpcMap 	s_rpc_map;
	RpcTimerMap s_rpc_timer_map;
	rpc_id_t s_rpc_id;
	OI::Thread::Mutex		_locker;
	OI::Thread::SpinLock	_rid_locker;
	RpcKeeper():s_rpc_id(1){}
	virtual ~RpcKeeper()
	{
		OI::Thread::MutexGuard l(_locker);
		for(RpcMap::iterator it = s_rpc_map.begin(); it != s_rpc_map.end(); ++it)
		{
			SAFE_DELETE(it->second);
		}
		s_rpc_map.clear();
	}
public:
	rpc_id_t GetRpcID();
	Rpc* FindAndRemoveStub(rpc_id_t rid);
	Rpc* FindStub(rpc_id_t rid);
	void DestoryStub(rpc_id_t rid);
	void AddStub(Rpc*);
};


#endif
