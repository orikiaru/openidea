#include <cstdio>
#include <cstring>
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
	const char* pConfigPath = "../config/logd.toml";
	int32_t iCode = ::OI::Base::ParseConfigArgument(iArgumentCount, vArguments, pConfigPath);
	if (iCode != OI_OK)
	{
		return iCode;
	}
	if (iArgumentCount == 2 && std::strcmp(vArguments[1], "--test-hooks") == 0)
	{
#ifdef OI_ENABLE_TEST_HOOKS
		std::printf("log_fault=1\n");
		return 41;
#else
		std::printf("log_fault=0\n");
		return 40;
#endif
	}
	iCode = ::OI::Base::StartFramework(pConfigPath);
	if (iCode != OI_OK)
	{
		fprintf(stderr, "StartFramework failed: code=%d\n", iCode);
		return iCode;
	}

	LogManager* pLogManager = nullptr;
	iCode = ::OI::Base::RunFramework([&pLogManager]() -> int32_t
	{
		// 写线程起来之前的日志只打 stdout
		pLogManager = &LogManager::GetInstance();
		int32_t iStatus = pLogManager->Init();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "LogManager Init failed: code=%d\n", iStatus);
			return iStatus;
		}
		::OI::Log::InitLocal(WriteSelfLog);
		iStatus = LogServer::GetInstance().Init();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "LogServer Init failed: code=%d\n", iStatus);
			return iStatus;
		}
		iStatus = LogServer::GetInstance().StartServer();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "LogServer StartServer failed: code=%d\n", iStatus);
			return iStatus;
		}
		return OI_OK;
	});
	::OI::Log::InitLocal(NULL);
	if (pLogManager)
	{
		pLogManager->Stop();
	}
	return iCode;
}
