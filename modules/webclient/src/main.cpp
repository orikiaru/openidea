#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "web_client.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	if (!::OI::Base::Config::GlobalMutableConfig().Parse("../config/webclient.toml"))
	{
		printf("ParseConfigError\n");
		return -2;
	}
	::OI::Thread::ThreadPool::GetInstance().Init();
	::OI::Timer::BaseTimerManager::GetInstance().Init();
	::OI::Net::Poll::GetInstance().Init();
	WebClient::GetInstance().Start();
	::OI::Thread::ThreadPool::GetInstance().StartWork();
	return ::OI::Thread::ThreadPool::GetInstance().ExitCode();
}
