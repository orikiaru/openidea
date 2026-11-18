#include <stdio.h>
#include <exception>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcclient.h"
#include "hold_mgr.h"

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
		::OI::Base::StopFramework();
		return iCode;
	}

	iCode = [&]() -> int32_t
	{
		try
		{
			iCode = IpcClient::GetInstance().Init();
			if (iCode != OI_OK)
			{
				fprintf(stderr, "IpcClient Init failed: code=%d\n", iCode);
				return iCode;
			}
			if (HoldMgr::GetInstance().RunsWithoutIpc())
			{
				HoldMgr::GetInstance().Start(INVALID_SID);
			}
			else
			{
				IpcClient::GetInstance().StartClient();
			}
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
