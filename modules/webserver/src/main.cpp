#include <stdio.h>
#include "base_header.h"
#include "web_server.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	if (!OI::Base::Config::GlobalMutableConfig().Parse("../config/webserver.toml"))
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
