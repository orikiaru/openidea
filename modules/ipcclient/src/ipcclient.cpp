#include "ipcclient.h"
#include "echo_mgr.h"
#include "log_mgr.h"

IpcClient IpcClient::inst;

IpcClient::~IpcClient()
{
}

bool IpcClient::Init()
{
	return SessionManager::Init();
}

const std::string IpcClient::GetIDString()
{
	return "IpcClient";
}

void IpcClient::OnClientConnect(Session* pSession)
{
	EchoMgr::Get().Load();
	LogMgr::Get().Load();
	EchoMgr::Get().ArmTimeout();
	EchoMgr::Get().Start(pSession->GetSessionID());
	LogMgr::Get().Start();
}

void IpcClient::OnClientDisconnect(const Session* pSession)
{
	(void)pSession;
}

void IpcClient::OnAbortSession()
{
}
