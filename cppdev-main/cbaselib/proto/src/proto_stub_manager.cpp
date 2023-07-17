#include "proto_stub_manager.h"
#include "assert.h"
#include "protocol.h"
void ProtoStubManager::RegisterStub(Protocol* p)
{
	assert(_proto_map.find(p->GetID()) == _proto_map.end());
	_proto_map[p->GetID()] = p;
}

ProtoStubManager::~ProtoStubManager()
{
	for (ProtoMap::iterator it = _proto_map.begin(); it != _proto_map.end(); ++it)
	{
		delete it->second;
	}
	_proto_map.clear();
}

static void _____RegisterStub() FUNCTION_RUN_BEFORE_MAIN;
static void _____RegisterStub()
{
	//TODO 注册一些通用协议结构
}
