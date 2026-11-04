#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "log_server.h"
#include "log_manager.h"

// logd 自己的日志直接进本地文件，不作为日志客户端连自己。
static bool WriteSelfLog(LOG_LEVEL iLevelParam, const char* pTextParam, size_t iLengthParam)
{
	return LogManager::GetInstance().WriteLog(iLevelParam, pTextParam, iLengthParam);
}

int main(int iArgumentCountParam, char* vArgumentsParam[])
{
	int iCodeLocal = ::OI::Base::StartFramework("../config/logd.toml");
	if (iCodeLocal != 0)
	{
		return iCodeLocal;
	}

	// 写线程起来之前的日志只打 stdout
	if (!LogManager::GetInstance().Init())
	{
		printf("LogManager Init error\n");
		return -3;
	}
	::OI::Log::InitLocal(WriteSelfLog);

	if (!LogServer::GetInstance().Init())
	{
		LOG_ERROR("LogServer::Init error");
		::OI::Log::InitLocal(NULL);
		LogManager::GetInstance().Stop();
		return -1;
	}
	if (!LogServer::GetInstance().StartServer())
	{
		LOG_ERROR("LogServer::StartServer error");
		::OI::Log::InitLocal(NULL);
		LogManager::GetInstance().Stop();
		return -1;
	}

	iCodeLocal = ::OI::Base::RunFramework();

	//网络和工人都停了，不会再进日志。写线程把剩下的写完再退出。
	::OI::Log::InitLocal(NULL);
	LogManager::GetInstance().Stop();
	return iCodeLocal;
}
