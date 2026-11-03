#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcclient.h"

int main(int iArgumentCountParam, char* vArgumentsParam[])
{
	int iCodeLocal = ::OI::Base::StartFramework("../config/ipcclient.toml");
	if (iCodeLocal != 0)
	{
		return iCodeLocal;
	}
	::OI::Log::Init();

	if (!IpcClient::GetInstance().Init())
	{
		printf("IpcClient Init error\n");
		return -1;
	}
	IpcClient::GetInstance().StartClient();

	return ::OI::Base::RunFramework();
}
