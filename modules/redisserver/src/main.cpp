#include <stdio.h>
#include <exception>
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
		::OI::Base::StopFramework();
		return iCode;
	}

	iCode = [&]() -> int32_t
	{
		try
		{
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
		}
		catch (const std::exception& stError)
		{
			fprintf(stderr, "module init failed: %s\n", stError.what());
			fflush(stderr);
			return OI_ERR_FAIL;
		}
		catch (...)
		{
			fprintf(stderr, "module init failed: unknown exception\n");
			fflush(stderr);
			return OI_ERR_FAIL;
		}
		return OI_OK;
	}();
	if (iCode != OI_OK)
	{
		::OI::Base::StopFramework();
		return iCode;
	}

	return ::OI::Base::RunFramework();
}
