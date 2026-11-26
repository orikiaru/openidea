#include <stdio.h>
#include <exception>
#include "oi_net.h"
#include "oi_base.h"
#include "web_client.h"

int main(int iArgumentCount, char* vArguments[])
{
	int32_t iCode = ::OI::Base::StartFramework("../config/webclient.toml", iArgumentCount, vArguments);
	if (iCode != OI_OK)
	{
		fprintf(stderr, "StartFramework failed: code=%d\n", iCode);
		return iCode;
	}
	iCode = [&]() -> int32_t
	{
		try
		{
			WebClient::GetInstance().Start();
		}
		catch (const std::exception& stError)
		{
			fprintf(stderr, "module init failed: %s\n", stError.what());
			fflush(stderr);
			return OI_ERR_FAIL;
		}
		catch (...)
		{
			fprintf(stderr, "module init failed: unknown exception\n");
			fflush(stderr);
			return OI_ERR_FAIL;
		}
		return OI_OK;
	}();
	if (iCode != OI_OK)
	{
		::OI::Base::StopFramework();
		return iCode;
	}
	return ::OI::Base::RunFramework();
}
