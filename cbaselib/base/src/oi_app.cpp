#include "oi_app.h"
#include <stdio.h>
#include "oi_config.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"

namespace OI
{
namespace Base
{
int StartFramework(const char* pConfigPathParam)
{
	if (!Config::GlobalMutableConfig().Parse(pConfigPathParam))
	{
		printf("ParseConfigError\n");
		return -2;
	}
	if (!::OI::Thread::ThreadPool::GetInstance().Init() || !::OI::Timer::BaseTimerManager::GetInstance().Init() || !::OI::Net::Poll::GetInstance().Init())
	{
		printf("framework init error\n");
		return -1;
	}
	return 0;
}

int RunFramework()
{
	::OI::Thread::ThreadPool::GetInstance().StartWork();
	return ::OI::Thread::ThreadPool::GetInstance().ExitCode();
}
}
}
