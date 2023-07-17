#include "log_client.h"
#include "config.h"
#include "log_protocol.h"
#include "string.h"
LogClient LogClient::inst;

LogClient::~LogClient()
{
}

bool LogClient::Init()
{
	LOG_TRACE("LogClient::Init");
	return ProtoClientManager::Init();
}

void LogClient::OnClientConnect(Session* pSession)
{
	LOG_TRACE("LogClient::OnClientConnect sid=%d", pSession->GetSessionID());
}

void LogClient::OnClientDisconnect(const Session* pSession)
{
	LOG_TRACE("LogClient::OnClientDisconnect Will Reconnect sid=%d, errno=%d", pSession->GetSessionID(), pSession->GetError());
	Reconnect();
}

void LogClient::OnAbortSession()
{
	LOG_TRACE("LogClient::OnAbortSession Will Reconnect");
	Reconnect();
}