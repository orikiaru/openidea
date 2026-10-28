#include "ipcclient.h"
#include "echo_mgr.h"
#include "hold_mgr.h"
#include "log_mgr.h"
#include "redis_mgr.h"
#include "redis_client.h"

IpcClient IpcClient::inst;

IpcClient::~IpcClient()
{
}

bool IpcClient::Init()
{
	if (!SessionManager::Init())
	{
		return false;
	}
	RedisMgr::Get().Load();
	HoldMgr::Get().Load();
	if (!RedisMgr::Get().Enabled() && !HoldMgr::Get().Enabled())
	{
		return true;
	}
	if (!RedisClient::GetInstance().Init())
	{
		return false;
	}
	RedisClient::GetInstance().StartClient();
	if (RedisMgr::Get().Enabled())
	{
		RedisMgr::Get().Start();
	}
	return true;
}

const std::string IpcClient::GetIDString()
{
	return "IpcClient";
}

void IpcClient::OnClientConnect(Session* pSession)
{
	EchoMgr::Get().Load();
	LogMgr::Get().Load();
	EchoMgr::Get().Start(pSession->GetSessionID());
	LogMgr::Get().Start();
	HoldMgr::Get().Start(pSession->GetSessionID());
}

void IpcClient::OnClientDisconnect(const Session* pSession)
{
	(void)pSession;
}

void IpcClient::OnAbortSession()
{
}
