#include <stdio.h>
#include "oi_base.h"
#include "web_server.h"

int main(int iArgumentCount, char* vArguments[])
{
	(void)iArgumentCount;
	(void)vArguments;
	int32_t iCode = ::OI::Base::Config::GlobalMutableConfig().Parse("../config/webserver.toml");
	if (iCode != OI_OK)
	{
		fprintf(stderr, "web server config parse failed: code=%d\n", iCode);
		return iCode;
	}
	iCode = WebServer::GetInstance().Run();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "web server run failed: code=%d\n", iCode);
		return iCode;
	}
	return 0;
}
