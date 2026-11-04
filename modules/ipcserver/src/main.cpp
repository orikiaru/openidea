#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcserver.h"

int main(int iArgumentCountParam, char* vArgumentsParam[])
{
	const char* pConfigPath = "../config/ipcserver.toml";
	if (iArgumentCountParam > 1 && vArgumentsParam[1] != NULL && vArgumentsParam[1][0] != '\0')
	{
		pConfigPath = vArgumentsParam[1];
	}
	int iCodeLocal = ::OI::Base::StartFramework(pConfigPath);
	if (iCodeLocal != 0)
	{
		return iCodeLocal;
	}
	::OI::Log::Init();

	if (!IpcServer::GetInstance().Init())
	{
		printf("IpcServer Init error\n");
		return -1;
	}
	if (!IpcServer::GetInstance().StartServer())
	{
		printf("IpcServer StartServer error\n");
		return -1;
	}
	printf("ipc server listen\n");
	fflush(stdout);
	IpcServer::GetInstance().StartHttpProbe();

	return ::OI::Base::RunFramework();
}
