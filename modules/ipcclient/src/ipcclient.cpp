#include "ipcclient.h"
#include "echo_mgr.h"
#include "hold_mgr.h"
#include "log_mgr.h"
#include "redis_mgr.h"
#include "redis_client.h"

IpcClient::~IpcClient()
{
}

bool IpcClient::Init()
{
	if (!SessionManager::Init())
	{
		return false;
	}
	RedisMgr::GetInstance().Load();
	HoldMgr::GetInstance().Load();
	if (!RedisMgr::GetInstance().Enabled() && !HoldMgr::GetInstance().Enabled())
	{
		return true;
	}
	if (!RedisClient::GetInstance().Init())
	{
		return false;
	}
	RedisClient::GetInstance().StartClient();
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

void IpcClient::OnClientConnect(Session* pSession)
{
	EchoMgr::GetInstance().Load();
	LogMgr::GetInstance().Load();
	EchoMgr::GetInstance().Start(pSession->GetSessionID());
	LogMgr::GetInstance().Start();
	HoldMgr::GetInstance().Start(pSession->GetSessionID());
}

void IpcClient::OnClientDisconnect(const Session* pSession)
{
	(void)pSession;
}

void IpcClient::OnAbortSession()
{
}
