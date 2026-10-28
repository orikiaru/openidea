#include "ipcclient.h"
#include "echo_mgr.h"
#include "hold_mgr.h"
#include "log_mgr.h"
#include "redis_mgr.h"
#include "oi_redis.h"

IpcClient::~IpcClient()
{
}

bool IpcClient::Init()
{
	if (!::OI::Net::SessionManager::Init())
	{
		return false;
	}
	RedisMgr::GetInstance().Load();
	HoldMgr::GetInstance().Load();
	if (!RedisMgr::GetInstance().Enabled() && !HoldMgr::GetInstance().Enabled())
	{
		return true;
	}
	if (!::OI::Redis::RedisClient::GetInstance().Init())
	{
		return false;
	}
	::OI::Redis::RedisClient::GetInstance().StartClient();
	if (RedisMgr::GetInstance().Enabled())
	{
		RedisMgr::GetInstance().Start();
	}
	return true;
}

const std::string IpcClient::GetIDString()
{
	return "IpcClient";
}

void IpcClient::OnClientConnect(::OI::Net::Session* pSession)
{
	EchoMgr::GetInstance().Load();
	LogMgr::GetInstance().Load();
	EchoMgr::GetInstance().Start(pSession->GetSessionID());
	LogMgr::GetInstance().Start();
	HoldMgr::GetInstance().Start(pSession->GetSessionID());
}

void IpcClient::OnClientDisconnect(const ::OI::Net::Session* pSession)
{
	(void)pSession;
}

void IpcClient::OnAbortSession()
{
}
