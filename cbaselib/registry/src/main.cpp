#include <stdio.h>
#include <exception>
#include "oi_net.h"
#include "oi_base.h"
#include "oi_poll.h"
#include "registry_server.h"

int main(int iArgumentCount, char* vArguments[])
{
	(void)iArgumentCount;
	(void)vArguments;
	int32_t iCode = ::OI::Base::StartFramework("../config/registry.toml");
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

	try
	{
		iCode = RegistryServer::GetInstance().Init();
		if (iCode != OI_OK)
		{
			fprintf(stderr, "RegistryServer Init failed: code=%d\n", iCode);
			return iCode;
		}
		iCode = RegistryServer::GetInstance().StartServer();
		if (iCode != OI_OK)
		{
			fprintf(stderr, "RegistryServer StartServer failed: code=%d\n", iCode);
			return iCode;
		}
		printf("registry server listen\n");
		fflush(stdout);
	}
	catch (const std::exception& stError)
	{
		fprintf(stderr, "module init failed: %s\n", stError.what());
		fflush(stderr);
		::OI::Net::Poll::GetInstance().StopAndJoin();
		return OI_ERR_FAIL;
	}
	catch (...)
	{
		fprintf(stderr, "module init failed: unknown exception\n");
		fflush(stderr);
		::OI::Net::Poll::GetInstance().StopAndJoin();
		return OI_ERR_FAIL;
	}

	return ::OI::Base::RunFramework();
}
