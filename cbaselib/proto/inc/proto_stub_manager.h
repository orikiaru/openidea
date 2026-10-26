#pragma once
#include <map>
#include <header.h>

class Protocol;
class ProtoStubManager
{
	typedef std::map<int, Protocol*> ProtoMap;
	ProtoMap _proto_map;
public:
	virtual ~ProtoStubManager();

	//注册存根必须在进程正式执行之前,建议在main函数执行之前.后续不可添加
	void RegisterStub(Protocol* p);
	Protocol* GetStub(proto_type_t type)
	{
		ProtoMap::iterator it = _proto_map.find(type);
		return it != _proto_map.end() ? it->second : NULL;
	}
};

//应用层自定义协议的存根管理器
class ProtocolStubs : public ProtoStubManager
{
public:
	static ProtocolStubs& GetInstance()
	{
		//这里必须如此声明,因为构建存根的时候需要保证构造ProtocolStubs在前
		static ProtocolStubs inst;
		return inst;
	}
};