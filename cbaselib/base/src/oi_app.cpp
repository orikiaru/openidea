#include "oi_app.h"
#include <stdio.h>
#include "oi_config.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_singleton.h"

namespace OI
{
namespace Base
{
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

int RunFramework()
{
	SingletonUpdates::Instance().Start();
	::OI::Thread::ThreadPool::GetInstance().StartWork();
	return ::OI::Thread::ThreadPool::GetInstance().ExitCode();
}
}
}
