#include <stdio.h>
#include "net_header.h"
#include "base_header.h"
#include "redis_server.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	if (!Config::GlobalMutableConfig().Parse("../config/redisserver.toml"))
	{
		printf("ParseConfigError\n");
		return -2;
	}

	ThreadPool::GetInstance()->Init();
	BaseTimerManager::GetInstance()->Init();
	Poll::GetInstance()->Init();
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

	ThreadPool::GetInstance()->StartWork();
	return 0;
}
