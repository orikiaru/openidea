#include <stdio.h>
#include "oi_base.h"
#include "web_server.h"

int main(int iArgumentCount, char* vArguments[])
{
	(void)iArgumentCount;
	(void)vArguments;
	if (!::OI::Base::Config::GlobalMutableConfig().Parse("../config/webserver.toml"))
	{
		printf("ParseConfigError\n");
		return -2;
	}
	if (!WebServer::GetInstance().Run())
	{
		printf("web server failed\n");
		return 1;
	}
	return 0;
}
