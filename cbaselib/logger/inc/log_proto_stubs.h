#pragma once
#include "proto_stub_manager.h"
#include "singleton.h"
#include "log_protocol.h"

class LogProtoStubs : public ProtoStubManager
{
	DECLARE_SINGLETON(LogProtoStubs)
	LogProtoStubs() {}
};