#include <cstdio>
#include <exception>
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
	iCode = [&]() -> int32_t
	{
		try
		{
			// 写线程起来之前的日志只打 stdout
			pLogManager = &LogManager::GetInstance();
			iCode = pLogManager->Init();
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
				return iCode;
			}
			iCode = LogServer::GetInstance().StartServer();
			if (iCode != OI_OK)
			{
				fprintf(stderr, "LogServer StartServer failed: code=%d\n", iCode);
				return iCode;
			}
		}
		catch (const std::exception& stError)
		{
			fprintf(stderr, "module init failed: %s\n", stError.what());
			fflush(stderr);
			return OI_ERR_FAIL;
		}
		catch (...)
		{
			fprintf(stderr, "module init failed: unknown exception\n");
			fflush(stderr);
			return OI_ERR_FAIL;
		}
		return OI_OK;
	}();
	if (iCode != OI_OK)
	{
		::OI::Base::StopFramework();
		::OI::Log::InitLocal(NULL);
		if (pLogManager)
		{
			pLogManager->Stop();
		}
		return iCode;
	}

	iCode = ::OI::Base::RunFramework();

	//网络和工人都停了，不会再进日志。写线程把剩下的写完再退出。
	::OI::Log::InitLocal(NULL);
	LogManager::GetInstance().Stop();
	return iCode;
}
