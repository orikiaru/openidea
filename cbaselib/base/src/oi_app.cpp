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
int StartFramework(const char* pConfigPath)
{
	if (!Config::GlobalMutableConfig().Parse(pConfigPath))
	{
		fprintf(stderr, "framework init failed: parse config %s\n", pConfigPath ? pConfigPath : "(null)");
		fflush(stderr);
		return -2;
	}
	if (!::OI::Thread::ThreadPool::GetInstance().Init())
	{
		fprintf(stderr, "framework init failed: thread pool; see [ThreadPool] threadcount in %s\n", pConfigPath);
		fflush(stderr);
		return -1;
	}
	if (!::OI::Timer::BaseTimerManager::GetInstance().Init())
	{
		fprintf(stderr, "framework init failed: timer manager; see [Timer] in %s\n", pConfigPath);
		fflush(stderr);
		return -1;
	}
	if (!::OI::Net::Poll::GetInstance().Init())
	{
		fprintf(stderr, "framework init failed: poll in %s\n", pConfigPath);
		fflush(stderr);
		return -1;
	}
	return 0;
}

int RunFramework()
{
	SingletonUpdates::Instance().Start();
	::OI::Thread::ThreadPool::GetInstance().StartWork();
	return ::OI::Thread::ThreadPool::GetInstance().ExitCode();
}
}
}
