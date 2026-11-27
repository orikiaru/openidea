#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcserver.h"
#include "ipcudp.h"

int main(int iArgumentCount, char* vArguments[])
{
	const char* pConfigPath = nullptr;
	int32_t iCode = ::OI::Base::ParseConfigArgument(iArgumentCount, vArguments, pConfigPath);
	if (iCode != OI_OK)
	{
		return iCode;
	}
	if (!pConfigPath)
	{
		pConfigPath = iArgumentCount > 1 && vArguments[1][0] ? vArguments[1] : "../config/ipcserver.toml";
	}
	iCode = ::OI::Base::StartFramework(pConfigPath);
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
		iStatus = IpcServer::GetInstance().Init();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "IpcServer Init failed: code=%d\n", iStatus);
			return iStatus;
		}
		iStatus = IpcServer::GetInstance().StartServer();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "IpcServer StartServer failed: code=%d\n", iStatus);
			return iStatus;
		}
		iStatus = IpcUdp::GetInstance().Init();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "IpcUdp Init failed: code=%d\n", iStatus);
			return iStatus;
		}
		iStatus = IpcUdp::GetInstance().StartServer();
		if (iStatus != OI_OK)
		{
			fprintf(stderr, "IpcUdp StartServer failed: code=%d\n", iStatus);
			return iStatus;
		}
		printf("ipc udp listen port=%u\n", (unsigned)IpcUdp::GetInstance().LocalPort());
		printf("ipc server listen\n");
		fflush(stdout);
		IpcServer::GetInstance().StartHttpProbe();
		return OI_OK;
	});
}
