#include "testclient.h"
#include "testprotocol.hpp"
#include "testprotocol2.hpp"
#include "testrpc.hrp"
#include "timer_task.h"
#include "base_timer_manager.h"
#include "testclient.h"
#include "net_header.h"

TestClient TestClient::inst;

TestClient::~TestClient()
{

}

bool TestClient::Init()
{
	return SessionManager::Init();
}

const std::string TestClient::GetIDString()
{
    return "TestClient";
}

class TestClientTestTask : public TimerTask
{
public:
	virtual void Handle()
	{
		int test_client_rpc_count = 10000;
		int test_client_proto_count = 10000;
		TryGetConfigNoReturn("Test.test_client_rpc_count", test_client_rpc_count);
		TryGetConfigNoReturn("Test.test_client_rpc_count", test_client_proto_count);
		for (int i = 0; i < test_client_rpc_count; ++i)
		{
			TestProtocol2 p;
			p.test_data = i;
			TestClient::GetInstance().SendProtocol(p);
		}
		for (int i = 0; i < test_client_proto_count; ++i)
		{
			TestReq req;
			req.a = 1 * i;
			req.b = 10 * i;
			req.c = 100 * i;
			TestRpc* pRpc = RpcCall(TestRpc, req);
			TestClient::GetInstance().SendProtocol(pRpc);
		}
		BaseTimerManager::GetInstance()->AddTimer(new TestClientTestTask, 1000);
	}
};

void TestClient::OnClientConnect(Session* pSession)
{
	bool test_net = false;
	TryGetConfigNoReturn("Test.test_net", test_net);
	if (test_net)
	{
		BaseTimerManager::GetInstance()->AddTimer(new TestClientTestTask, 1000);
	}
	LOG_TRACE("TestClient::OnClientConnect sid=%d\n", pSession->GetSessionID());
}

void TestClient::OnClientDisconnect(const Session* pSession)
{
	LOG_TRACE("TestClient::OnClientDisconnect sid=%d\n", pSession->GetSessionID());
	Reconnect();
}

void TestClient::OnAbortSession()
{
	LOG_TRACE("TestClient::OnAbortSession");
	Reconnect();
}        
