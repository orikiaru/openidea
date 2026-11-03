#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "registry_server.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	int code = ::OI::Base::StartFramework("../config/registry.toml");
	if (code != 0)
	{
		return code;
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
