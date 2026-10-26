#pragma once
#include "proto_stub_manager.h"
#include "log_protocol.h"

class LogProtoStubs : public ProtoStubManager
{
public:
	LogProtoStubs() {}

	static LogProtoStubs& GetInstance() 
	{ 
		static LogProtoStubs inst;
		return inst; 
	}
};