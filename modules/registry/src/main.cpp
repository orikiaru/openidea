#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "oi_registry.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	if (!::OI::Base::Config::GlobalMutableConfig().Parse("../config/registry.toml"))
	{
		printf("ParseConfigError\n");
		return -2;
	}

	::OI::Thread::ThreadPool::GetInstance().Init();
	::OI::Timer::BaseTimerManager::GetInstance().Init();
	::OI::Net::Poll::GetInstance().Init();
	::OI::Log::Init();

	if (!::OI::Base::RegistryServer::GetInstance().Init())
	{
		printf("RegistryServer Init error\n");
		return -1;
	}
	if (!::OI::Base::RegistryServer::GetInstance().StartServer())
	{
		printf("RegistryServer StartServer error\n");
		return -1;
	}
	printf("registry server listen\n");
	fflush(stdout);

	::OI::Thread::ThreadPool::GetInstance().StartWork();
	return 0;
}
