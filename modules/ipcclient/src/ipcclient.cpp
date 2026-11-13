#include "ipcclient.h"
#include "echo_mgr.h"
#include "coro_mgr.h"
#include "db_mgr.h"
#include "hold_mgr.h"
#include "log_mgr.h"
#include "multi_mgr.h"
#include "redis_mgr.h"
#include "udp_mgr.h"
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
	DbMgr::GetInstance().Load();
	EchoMgr::GetInstance().Load();
	CoroMgr::GetInstance().Load();
	if (EchoMgr::GetInstance().StackOn())
	{
		OI::Coro::StartTask([](OI::Coro::Token)
		{
			return EchoMgr::GetInstance().StackProbe();
		});
	}
	iCode = UdpMgr::GetInstance().Load();
	if (iCode != OI_OK)
	{
		LOG_ERROR("ipcclient init udp load failed code=%d", iCode);
		return iCode;
	}
	UdpMgr::GetInstance().Start();
	iCode = MultiMgr::GetInstance().Load();
	if (iCode != OI_OK)
	{
		LOG_ERROR("ipcclient init multi load failed code=%d", iCode);
		return iCode;
	}
	MultiMgr::GetInstance().Start();
	RedisMgr::GetInstance().Load();
	HoldMgr::GetInstance().Load();
	if (!RedisMgr::GetInstance().Enabled() && !HoldMgr::GetInstance().Enabled())
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
	if (RedisMgr::GetInstance().Enabled())
	{
		RedisMgr::GetInstance().Start();
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
	EchoMgr::GetInstance().Load();
	LogMgr::GetInstance().Load();
	EchoMgr::GetInstance().Start(pSession->GetSessionID());
	CoroMgr::GetInstance().Start();
	LogMgr::GetInstance().Start();
	HoldMgr::GetInstance().Start(pSession->GetSessionID());
	DbMgr::GetInstance().Start();
}

void IpcClient::OnClientDisconnect(const ::OI::Net::Session* pSession)
{
	(void)pSession;
}
