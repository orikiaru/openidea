#include "ipcclient.h"
#include "test_echo.h"
#include "test_coro.h"
#include "test_empty.h"
#include "test_db.h"
#include "test_hold.h"
#include "test_log.h"
#include "test_multi.h"
#include "test_redis.h"
#include "test_udp.h"
#include "oi_redis.h"
#include "oi_log.h"

IpcClient::~IpcClient()
{
}

int32_t IpcClient::Init()
{
	int32_t iCode = ::OI::Rpc::ProtoSessionManager::Init();
	if (iCode != OI_OK)
	{
		LOG_ERROR("ipcclient init protocol manager failed code=%d", iCode);
		return iCode;
	}
	TestDb::GetInstance().Load();
	TestEcho::GetInstance().Load();
	TestCoro::GetInstance().Load();
	if (TestEcho::GetInstance().StackOn())
	{
		OI::Coro::StartTask([]()
		{
			return TestEcho::GetInstance().StackProbe();
		});
	}
	iCode = TestUdp::GetInstance().Load();
	if (iCode != OI_OK)
	{
		LOG_ERROR("ipcclient init udp load failed code=%d", iCode);
		return iCode;
	}
	TestUdp::GetInstance().Start();
	iCode = TestMulti::GetInstance().Load();
	if (iCode != OI_OK)
	{
		LOG_ERROR("ipcclient init multi load failed code=%d", iCode);
		return iCode;
	}
	TestMulti::GetInstance().Start();
	TestRedis::GetInstance().Load();
	iCode = TestHold::GetInstance().Load();
	if (iCode != OI_OK)
	{
		return iCode;
	}
	if (!TestRedis::GetInstance().Enabled() && !TestHold::GetInstance().NeedsRedis())
	{
		return OI_OK;
	}
	iCode = ::OI::Redis::RedisClient::GetInstance().Init();
	if (iCode != OI_OK)
	{
		LOG_ERROR("ipcclient init redis failed code=%d", iCode);
		return iCode;
	}
	::OI::Redis::RedisClient::GetInstance().StartClient();
	if (TestRedis::GetInstance().Enabled())
	{
		TestRedis::GetInstance().Start();
	}
	return OI_OK;
}

const std::string IpcClient::GetIDString()
{
	return "IpcClient";
}

void IpcClient::OnClientConnect(::OI::Net::Session* pSession)
{
	// 服务发现切换或重连可能建立多个会话；每个进程只启动一次测试。
	if (m_bTestsStarted.exchange(true))
	{
		return;
	}
	TestEcho::GetInstance().Load();
	TestLog::GetInstance().Load();
	TestEcho::GetInstance().Start(pSession->GetSessionID());
	TestCoro::GetInstance().Start();
	TestEmpty::GetInstance().Load();
	TestEmpty::GetInstance().Start();
	TestLog::GetInstance().Start();
	TestHold::GetInstance().Start(pSession->GetSessionID());
	TestDb::GetInstance().Start();
}

void IpcClient::OnClientDisconnect(const ::OI::Net::Session* pSession)
{
	(void)pSession;
}
