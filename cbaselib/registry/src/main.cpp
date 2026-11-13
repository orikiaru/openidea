#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
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

	return ::OI::Base::RunFramework();
}
