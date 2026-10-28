#include <stdio.h>
#include "base_header.h"
#include "web_client.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	if (!OI::Base::Config::GlobalMutableConfig().Parse("../config/webclient.toml"))
	{
		printf("ParseConfigError\n");
		return -2;
	}
	return WebClient::GetInstance().Run();
}
