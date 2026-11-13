#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcclient.h"

int main(int iArgumentCount, char* vArguments[])
{
	int32_t iCode = ::OI::Base::StartFramework("../config/ipcclient.toml");
	if (iCode != OI_OK)
	{
		fprintf(stderr, "StartFramework failed: code=%d\n", iCode);
		return iCode;
	}
	iCode = ::OI::Log::Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "Log Init failed: code=%d\n", iCode);
		return iCode;
	}

	iCode = IpcClient::GetInstance().Init();
	if (iCode != OI_OK)
	{
		fprintf(stderr, "IpcClient Init failed: code=%d\n", iCode);
		return iCode;
	}
	IpcClient::GetInstance().StartClient();

	return ::OI::Base::RunFramework();
}
