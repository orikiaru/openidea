#ifndef __jxy_rpc_h__
#define __jxy_rpc_h__

#include "protocol.h"
#include "base_timer.h"

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

class RpcData: public Serialize
{
public:
	virtual BytesStream& serialize(BytesStream& os) const = 0;
	virtual BytesStream& deserialize(BytesStream& os) = 0;
	virtual RpcData* Clone() = 0;
};

static bool IsReqID(rpc_id_t rid) 		{ return rid & RPC_REQ_MASK; }
static void ClearReq(rpc_id_t& rid) 	{ rid &= ~RPC_REQ_MASK;}

class Rpc : public TimerProtocol
{
protected:
	rpc_id_t _rid;
	RpcData* _req;
	RpcData* _res;
	Rpc*	 _p_stub;	//rpc回执时的临时存根保存,不再去RpcKeeper重复获取了
public:
	Rpc(proto_type_t type, RpcData* req, RpcData* res) :TimerProtocol(type), _rid(0), _req(req), _res(res), _p_stub(NULL){}
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

	virtual BytesStream& serialize(BytesStream& os) const;
	virtual BytesStream& deserialize(BytesStream& os);
	
	//设置req的纯虚函数,子类检查reqtype是否匹配
	virtual void SetReq(RpcData* req)
	{
		assert(false);	//需要检查Req类型是否匹配,不走基类,基类给个示例
		SAFE_DELETE(_req);
		_req = req;
	}

	//以下是模块自定义协议处理
	virtual void DoRequest(SessionManager* mgr, sid_t sid) = 0;
	virtual void DoResponse(SessionManager* mgr, sid_t sid) = 0;
	//实现的RPC实例通过重写req res序列化与反序列化实现具体rpc内容
	virtual BytesStream& serialize_req(BytesStream& os) const	{ return os << *_req; }
	virtual BytesStream& deserialize_req(BytesStream& os)		{ return os >> *_req; }
	virtual BytesStream& serialize_res(BytesStream& os) const	{ return os << *_res; }
	virtual BytesStream& deserialize_res(BytesStream& os)		{ return os >> *_res; }

	static Rpc* Call(proto_type_t id, RpcData* p_data);
	static Rpc* Call(proto_type_t id, RpcData& p_data)	{	return Call(id, &p_data);	}

private:
	void SetResStub(Rpc* p_rpc) { _p_stub = p_rpc;  }
};

#define RpcCall(RPC_TYPE, req) (dynamic_cast<RPC_TYPE*>(Rpc::Call(RPC_TYPE::RpcType(), req)))
	

//可以考虑绑定到回话管理器上,一个会话管理器保存自身所有RPC存根,减少锁冲突
class RpcKeeper
{
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
	Mutex		_locker;
	SpinLock	_rid_locker;
	RpcKeeper():s_rpc_id(1){}
	virtual ~RpcKeeper()
	{
		MutexGuard l(_locker);
		for(RpcMap::iterator it = s_rpc_map.begin(); it != s_rpc_map.end(); ++it)
		{
			SAFE_DELETE(it->second);
		}
		s_rpc_map.clear();
	}
public:
	static RpcKeeper& GetInstance()
	{
		static RpcKeeper inst;
		return inst;
	}
	rpc_id_t GetRpcID();
	Rpc* FindAndRemoveStub(rpc_id_t rid);
	Rpc* FindStub(rpc_id_t rid);
	void DestoryStub(rpc_id_t rid);
	void AddStub(Rpc*);
	Rpc* CreateRpc(proto_type_t id);
};


#endif
