#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "web_client.h"

int main(int iArgumentCountParam, char* vArgumentsParam[])
{
	(void)iArgumentCountParam;
	(void)vArgumentsParam;
	int iCodeLocal = ::OI::Base::StartFramework("../config/webclient.toml");
	if (iCodeLocal != 0)
	{
		return iCodeLocal;
	}
	WebClient::GetInstance().Start();
	return ::OI::Base::RunFramework();
}
