#include <stdio.h>
#include <exception>
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
