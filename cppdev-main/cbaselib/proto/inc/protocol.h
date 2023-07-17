#ifndef __j_protocol_h__
#define __j_protocol_h__

#include "serialize.h"
#include "threadpool.h"
#include "proto_session.h"
#include "session_manager.h"


class SessionManager;

class Protocol: public Serialize
{
protected:
	proto_type_t	_id;
public:
	//不应有走默认构造函数
	explicit Protocol(){ 	assert(false);	}
	Protocol(const Protocol& that) {	_id = that.GetID();  }
	//构造时尝试注册,注册为全局静态初始化
	//正常存根Clone走赋值拷贝
	Protocol(proto_type_t id):_id(id){}
	virtual ~Protocol(){}
	proto_type_t GetID()	const{	return _id; 	}
	virtual Protocol* Clone() = 0;
	virtual void Handle(SessionManager* mgr, sid_t sid) = 0;
};







class PTask : public Task
{
	Protocol*		_p;
	SessionManager* _p_mgr;
	sid_t			_sid;
public:
	PTask(Protocol* p, SessionManager* mgr, sid_t sid) :_p(p), _p_mgr(mgr), _sid(sid) {}
	virtual void Exec() 
	{
		_p->Handle(_p_mgr, _sid);
		delete this; 
	}
	~PTask() 
	{ 
		SAFE_DELETE(_p); 
	}
};



#endif

