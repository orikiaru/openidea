#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "registry_server.h"

int main(int iArgumentCount, char* vArguments[])
{
	int32_t iCode = ::OI::Base::StartFramework("../config/registry.toml", iArgumentCount, vArguments);
	if (iCode != OI_OK)
	{
		fprintf(stderr, "StartFramework failed: code=%d\n", iCode);
		return iCode;
	}
	return ::OI::Base::RunFramework([]() -> int32_t
	{
		int32_t iStatus = ::OI::Log::Init();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "Log Init failed: code=%d\n", iStatus);
			return iStatus;
		}
		iStatus = RegistryServer::GetInstance().Init();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "RegistryServer Init failed: code=%d\n", iStatus);
			return iStatus;
		}
		iStatus = RegistryServer::GetInstance().StartServer();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "RegistryServer StartServer failed: code=%d\n", iStatus);
			return iStatus;
		}
		printf("registry server listen\n");
		fflush(stdout);
		return OI_OK;
	});
}
