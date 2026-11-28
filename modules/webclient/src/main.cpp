#include "oi_module_components.h"
#include <stdio.h>
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
	return ::OI::Base::RunFramework([]() -> int32_t
	{
		WebClient::GetInstance().Start();
		return ModuleComponents::Start();
	});
}
