#include <stdio.h>
#include "oi_base.h"
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
