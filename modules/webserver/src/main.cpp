#include "oi_base.h"
#include "web_server.h"
#include <cstdio>

int main(int iArgumentCount, char* vArguments[])
{
	int32_t iCode = OI::Base::StartFramework("../config/webserver.toml", iArgumentCount, vArguments);
	if (iCode != OI_OK)
	{
		return iCode;
	}
	return OI::Base::RunFramework([]() -> int32_t
	{
		auto& stServer = WebServer::GetInstance();
		int32_t iStatus = stServer.Init();
		if (iStatus == OI_OK)
		{
			iStatus = stServer.StartServer();
		}
		if (iStatus != OI_OK)
		{
			std::fprintf(stderr, "web server startup failed code=%d\n", iStatus);
			return iStatus;
		}
		std::printf("web server listen %s:%s\n", stServer.GetStrIP().c_str(), stServer.GetStrPort().c_str());
		std::fflush(stdout);
		return OI_OK;
	});
}
