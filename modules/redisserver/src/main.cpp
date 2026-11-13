#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "redis_server.h"

int main(int iArgumentCount, char* vArguments[])
{
	(void)iArgumentCount;
	(void)vArguments;
	int32_t iCode = ::OI::Base::StartFramework("../config/redisserver.toml");
	if (iCode != OI_OK)
	{
		fprintf(stderr, "StartFramework failed: code=%d\n", iCode);
		return iCode;
	}
	iCode = ::OI::Log::Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "Log Init failed: code=%d\n", iCode);
		return iCode;
	}

	iCode = RedisServer::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "RedisServer Init failed: code=%d\n", iCode);
		return iCode;
	}
	iCode = RedisServer::GetInstance().StartServer();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "RedisServer StartServer failed: code=%d\n", iCode);
		return iCode;
	}
	printf("redis server listen\n");
	fflush(stdout);

	return ::OI::Base::RunFramework();
}
