#include "test_net.h"
#include "testclient.h"
#include "testserver.h"
#include "config.h"
void TestNet::DoPerformanceTest()
{
	bool test_net = false;
	TryGetConfigNoReturn("Test.test_net", test_net);
	if(test_net)
	{
		if(!TestServer::GetInstance().Init())
		{
			printf("TestServer Init error\n");
			exit(-1);
		}
		if (!TestServer::GetInstance().StartServer())
		{
			printf("TestServer StartServer error\n");
			exit(-1);
		}   

		if (!TestClient::GetInstance().Init())
		{
			printf("TestClient Init error\n");
			exit(-1);
		}
		TestClient::GetInstance().StartClient();
	}
}
