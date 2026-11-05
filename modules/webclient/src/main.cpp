#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "web_client.h"

int main(int iArgumentCount, char* vArguments[])
{
	(void)iArgumentCount;
	(void)vArguments;
	int iCodeLocal = ::OI::Base::StartFramework("../config/webclient.toml");
	if (iCodeLocal != 0)
	{
		return iCodeLocal;
	}
	WebClient::GetInstance().Start();
	return ::OI::Base::RunFramework();
}
