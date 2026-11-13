#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcserver.h"
#include "ipcudp.h"

int main(int iArgumentCount, char* vArguments[])
{
	const char* pConfigPath = "../config/ipcserver.toml";
	if (iArgumentCount > 1 && vArguments[1] != NULL && vArguments[1][0] != '\0')
	{
		pConfigPath = vArguments[1];
	}
	int32_t iCode = ::OI::Base::StartFramework(pConfigPath);
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

	iCode = IpcServer::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "IpcServer Init failed: code=%d\n", iCode);
		return iCode;
	}
	iCode = IpcServer::GetInstance().StartServer();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "IpcServer StartServer failed: code=%d\n", iCode);
		return iCode;
	}
	iCode = IpcUdp::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "IpcUdp Init failed: code=%d\n", iCode);
		return iCode;
	}
	iCode = IpcUdp::GetInstance().StartServer();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "IpcUdp StartServer failed: code=%d\n", iCode);
		return iCode;
	}
	printf("ipc udp listen port=%u\n", (unsigned)IpcUdp::GetInstance().LocalPort());
	printf("ipc server listen\n");
	fflush(stdout);
	IpcServer::GetInstance().StartHttpProbe();

	return ::OI::Base::RunFramework();
}
