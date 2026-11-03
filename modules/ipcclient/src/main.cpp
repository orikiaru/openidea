#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcclient.h"

int main(int argc, char* argv[])
{
    int code = ::OI::Base::StartFramework("../config/ipcclient.toml");
    if (code != 0)
    {
        return code;
    }
    ::OI::Log::Init();

    if(!IpcClient::GetInstance().Init())
    {
        printf("IpcClient Init error\n");
        return -1;
    }
    IpcClient::GetInstance().StartClient();

    return ::OI::Base::RunFramework();
}
