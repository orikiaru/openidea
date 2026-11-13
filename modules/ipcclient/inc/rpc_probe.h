#pragma once
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "oi_rpc_session_manager.h"

class DeferredRpcProbe : public OI::Rpc::ProtoClientManager
{
  public:
	DeferredRpcProbe()
	{
		DefaultInit("127.0.0.1", HoldTest::IpcServerPort());
	}
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
	}
};

struct RpcOutputWait
{
	static OI::Coro::TaskResult<int> Await(OI::Coro::TaskResult<int32_t> stTask)
	{
		try
		{
			co_return (co_await stTask) == 0 ? 0 : 1;
		}
		catch (const std::exception&)
		{
			co_return 1;
		}
	}
};
