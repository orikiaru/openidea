#include "log_server.h"
#include "logger.h"

LogServer LogServer::inst;

LogServer::~LogServer()
{

}

bool LogServer::Init()
{
	//会话控制器只管理基础连接数据 ip.port.type
	//会话的基础数据如 buff大小后续也会接入到基础管理器

	//应用层自己设计的配置,需在此处读取
	return SessionManager::Init();
}

void LogServer::OnAddSession(Session* pSession)
{
	LOG_TRACE("LogServer::OnAddSession sid=%d\n", pSession->GetSessionID());
}

void LogServer::OnDelSession(const Session* pSession)
{
	LOG_TRACE("LogServer::OnDelSession sid=%d\n", pSession->GetSessionID());
}

