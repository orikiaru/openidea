#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "redis_server.h"

int main(int iArgumentCountParam, char* vArgumentsParam[])
{
	(void)iArgumentCountParam;
	(void)vArgumentsParam;
	int iCodeLocal = ::OI::Base::StartFramework("../config/redisserver.toml");
	if (iCodeLocal != 0)
	{
		return iCodeLocal;
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
