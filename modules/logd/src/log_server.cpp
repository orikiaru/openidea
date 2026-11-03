#include "log_server.h"
#include "oi_log.h"

LogServer::~LogServer()
{

}

bool LogServer::Init()
{
	return ::OI::Net::SessionManager::Init();
}

void LogServer::OnAddSession(::OI::Net::Session* pSession)
{
	LOG_TRACE("LogServer::OnAddSession sid=%d", pSession->GetSessionID());
}

void LogServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	LOG_TRACE("LogServer::OnDelSession sid=%d", pSession->GetSessionID());
}

