#ifndef __j_protocol_h__
#define __j_protocol_h__

#include "oi_rpc.h"
#include "oi_rpc_session.h"
#include "oi_session_manager.h"
namespace OI { namespace Rpc {



class Protocol: public ::OI::Rpc::Serialize
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
	virtual void Handle(::OI::Net::SessionManager* mgr, sid_t sid) = 0;
};

}}
#endif
