#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "registry_server.h"

int main(int iArgumentCount, char* vArguments[])
{
	(void)iArgumentCount;
	(void)vArguments;
	int iCodeLocal = ::OI::Base::StartFramework("../config/registry.toml");
	if (iCodeLocal != 0)
	{
		return iCodeLocal;
	}
	::OI::Log::Init();

	if (!RegistryServer::GetInstance().Init())
	{
		printf("RegistryServer Init error\n");
		return -1;
	}
	if (!RegistryServer::GetInstance().StartServer())
	{
		printf("RegistryServer StartServer error\n");
		return -1;
	}
	printf("registry server listen\n");
	fflush(stdout);

	return ::OI::Base::RunFramework();
}
