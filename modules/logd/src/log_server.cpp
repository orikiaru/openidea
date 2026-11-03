#include "log_server.h"
#include "oi_log.h"

LogServer::~LogServer()
{
}

bool LogServer::Init()
{
	return ::OI::Net::SessionManager::Init();
}

void LogServer::OnAddSession(::OI::Net::Session* pSessionParam)
{
	LOG_TRACE("LogServer::OnAddSession sid=%d", pSessionParam->GetSessionID());
}

void LogServer::OnDelSession(const ::OI::Net::Session* pSessionParam)
{
	LOG_TRACE("LogServer::OnDelSession sid=%d", pSessionParam->GetSessionID());
}
