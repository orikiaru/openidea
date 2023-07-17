#include "testserver.h"
#include "testprotocol.hpp"
#include "testprotocol2.hpp"
#include "testrpc.hrp"
#include "net_header.h"
#include "base_header.h"


TestServer TestServer::inst;

TestServer::~TestServer()
{

}

const std::string TestServer::GetIDString()
{
    return "TestServer";
}

bool TestServer::Init()
{
	return SessionManager::Init();
}

class TestServerTestTask : public TimerTask
{
public:
	virtual void Handle()
	{
		int test_server_rpc_count = 10000;
		int test_server_proto_count = 10000;
		TryGetConfigNoReturn("Test.test_server_rpc_count", test_server_rpc_count);
		TryGetConfigNoReturn("Test.test_server_proto_count", test_server_proto_count);
		for (int i = 0; i < test_server_rpc_count; ++i)
		{
			TestProtocol2 p;
			p.test_data = i;
			TestServer::GetInstance().BroadCast(p);
		}
		for (int i = 0; i < test_server_proto_count; ++i)
		{
			TestReq req;
			req.a = 1 * i;
			req.b = 10 * i;
			req.c = 100 * i;
			TestRpc* pRpc = RpcCall(TestRpc, req);
			TestServer::GetInstance().BroadCast(pRpc);
		}
		BaseTimerManager::GetInstance()->AddTimer(new TestServerTestTask, 1000);
	}
};


void TestServer::OnAddSession(Session* pSession)
{
	bool test_net = false;
	TryGetConfigNoReturn("Test.test_net", test_net);
	if (test_net)
	{
		BaseTimerManager::GetInstance()->AddTimer(new TestServerTestTask, 1000);
	}
	LOG_TRACE("TestServer::OnAddSession sid=%d\n", pSession->GetSessionID());
}

void TestServer::OnDelSession(const Session* pSession)
{
	LOG_TRACE("TestServer::OnDelSession sid=%d\n", pSession->GetSessionID());
}

