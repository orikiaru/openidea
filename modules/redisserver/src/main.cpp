#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "redis_server.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	int code = ::OI::Base::StartFramework("../config/redisserver.toml");
	if (code != 0)
	{
		return code;
	}
	::OI::Log::Init();

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

	return ::OI::Base::RunFramework();
}
