#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "redis_server.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	if (!OI::Base::Config::GlobalMutableConfig().Parse("../config/redisserver.toml"))
	{
		printf("ParseConfigError\n");
		return -2;
	}

	OI::Thread::ThreadPool::GetInstance().Init();
	BaseTimerManager::GetInstance().Init();
	Poll::GetInstance().Init();
	Log::Init();

	if (!RedisServer::GetInstance().Init())
	{
		printf("RedisServer Init error\n");
		return -1;
	}
	if (!RedisServer::GetInstance().StartServer())
	{
		printf("RedisServer StartServer error\n");
		return -1;
	}
	printf("redis server listen\n");
	fflush(stdout);

	OI::Thread::ThreadPool::GetInstance().StartWork();
	return 0;
}
