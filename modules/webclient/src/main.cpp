#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "web_client.h"

int main(int argc, char* argv[])
{
	(void)argc;
	(void)argv;
	int code = ::OI::Base::StartFramework("../config/webclient.toml");
	if (code != 0)
	{
		return code;
	}
	WebClient::GetInstance().Start();
	return ::OI::Base::RunFramework();
}
