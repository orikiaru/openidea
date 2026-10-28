#pragma once
#include <map>
#include <header.h>
#include "singleton.h"

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
	DECLARE_SINGLETON(ProtocolStubs)
	ProtocolStubs() {}
public:
	// 第一次取实例时才构造，保证比静态注册存根更早。
};