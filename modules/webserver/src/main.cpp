#include "oi_base.h"
#include "web_server.h"
#include <cstdio>
#include <exception>

int main()
{
	int32_t iCode = OI::Base::StartFramework("../config/webserver.toml");
	if (iCode != OI_OK)
	{
		return iCode;
	}
	auto& stServer = WebServer::GetInstance();
	try
	{
		iCode = stServer.Init();
		if (iCode == OI_OK)
		{
			iCode = stServer.StartServer();
		}
	}
	catch (const std::exception& stError)
	{
		std::fprintf(stderr, "web server startup failed: %s\n", stError.what());
		iCode = OI_ERR_FAIL;
	}
	if (iCode != OI_OK)
	{
		std::fprintf(stderr, "web server startup failed code=%d\n", iCode);
		OI::Base::StopFramework();
		return iCode;
	}
	std::printf("web server listen %s:%s\n", stServer.GetStrIP().c_str(), stServer.GetStrPort().c_str());
	std::fflush(stdout);
	return OI::Base::RunFramework();
}
