#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "log_server.h"
#include "log_manager.h"

// logd 自己的日志直接进本地文件，不作为日志客户端连自己。
static int32_t WriteSelfLog(LOG_LEVEL iLevel, const char* pText, size_t iLength)
{
	return LogManager::GetInstance().WriteLog(iLevel, pText, iLength);
}

int main(int iArgumentCount, char* vArguments[])
{
	int32_t iCode = ::OI::Base::StartFramework("../config/logd.toml");
	if (iCode != OI_OK)
	{
		fprintf(stderr, "StartFramework failed: code=%d\n", iCode);
		return iCode;
	}

	// 写线程起来之前的日志只打 stdout
	iCode = LogManager::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "LogManager Init failed: code=%d\n", iCode);
		return iCode;
	}
	::OI::Log::InitLocal(WriteSelfLog);

	iCode = LogServer::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "LogServer Init failed: code=%d\n", iCode);
		::OI::Log::InitLocal(NULL);
		LogManager::GetInstance().Stop();
		return iCode;
	}
	iCode = LogServer::GetInstance().StartServer();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "LogServer StartServer failed: code=%d\n", iCode);
		::OI::Log::InitLocal(NULL);
		LogManager::GetInstance().Stop();
		return iCode;
	}

	iCode = ::OI::Base::RunFramework();

	//网络和工人都停了，不会再进日志。写线程把剩下的写完再退出。
	::OI::Log::InitLocal(NULL);
	LogManager::GetInstance().Stop();
	return iCode;
}
