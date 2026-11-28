#include "oi_app.h"
#include <stdio.h>
#include <cstring>
#include <exception>
#include "oi_config.h"
#include "oi_thread.h"
#include "oi_framework_timer.h"
#include "oi_poll.h"
#include "oi_singleton.h"

namespace OI
{
namespace Base
{
int32_t ParseConfigArgument(int& iArgumentCount, char* vArguments[], const char*& pConfigPath)
{
	int iOption = 0;
	for (int iIndex = 1; iIndex < iArgumentCount; ++iIndex)
	{
		if (std::strcmp(vArguments[iIndex], "--config") != 0)
		{
			continue;
		}
		if (iOption || iIndex + 1 >= iArgumentCount || !vArguments[iIndex + 1][0] ||
		    std::strncmp(vArguments[iIndex + 1], "--", 2) == 0)
		{
			fprintf(stderr, "--config requires one nonempty path and may only be specified once\n");
			return OI_ERR_INVALID_ARGUMENT;
		}
		iOption = iIndex;
		++iIndex;
	}
	if (iOption)
	{
		pConfigPath = vArguments[iOption + 1];
		iArgumentCount -= 2;
		for (int iIndex = iOption; iIndex < iArgumentCount; ++iIndex)
		{
			vArguments[iIndex] = vArguments[iIndex + 2];
		}
		vArguments[iArgumentCount] = nullptr;
	}
	return OI_OK;
}

int32_t StartFramework(const char* pDefaultConfigPath, int& iArgumentCount, char* vArguments[])
{
	const int32_t iStatus = ParseConfigArgument(iArgumentCount, vArguments, pDefaultConfigPath);
	if (iStatus != OI_OK)
	{
		return iStatus;
	}
	return StartFramework(pDefaultConfigPath);
}

int32_t StartFramework(const char* pConfigPath)
{
	if (!pConfigPath)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	int32_t iCode = Config::GlobalMutableConfig().Parse(pConfigPath);
	if (iCode != OI_OK)
	{
		fprintf(stderr, "framework init failed: parse config %s code=%d\n", pConfigPath, iCode);
		fflush(stderr);
		return iCode;
	}
	iCode = ::OI::Thread::ThreadPool::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "framework init failed: thread pool; see [ThreadPool] threadcount in %s code=%d\n", pConfigPath, iCode);
		fflush(stderr);
		return iCode;
	}
	iCode = ::OI::Timer::BaseTimerManager::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "framework init failed: timer manager; see [Timer] in %s code=%d\n", pConfigPath, iCode);
		fflush(stderr);
		return iCode;
	}
	iCode = ::OI::Net::Poll::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "framework init failed: poll in %s code=%d\n", pConfigPath, iCode);
		fflush(stderr);
		return iCode;
	}
	return OI_OK;
}

void StopFramework()
{
	::OI::Net::Poll::GetInstance().StopAndJoin();
	::OI::Timer::FrameworkTimer::ReleasePendingAfterJoin();
}

int RunFramework()
{
	SingletonUpdates::Instance().Start();
	::OI::Thread::ThreadPool::GetInstance().StartWork();
	::OI::Timer::FrameworkTimer::ReleasePendingAfterJoin();
	return ::OI::Thread::ThreadPool::GetInstance().ExitCode();
}

int RunFramework(const std::function<int32_t()>& fnInit)
{
	int32_t iCode = OI_ERR_FAIL;
	try
	{
		iCode = fnInit();
	}
	catch (const std::exception& stError)
	{
		fprintf(stderr, "module init failed: %s\n", stError.what());
	}
	catch (...)
	{
		fprintf(stderr, "module init failed: unknown exception\n");
	}
	if (iCode != OI_OK)
	{
		StopFramework();
		return iCode;
	}
	return RunFramework();
}
}
}
