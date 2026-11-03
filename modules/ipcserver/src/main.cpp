#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcserver.h"

int main(int argc, char* argv[])
{
    int code = ::OI::Base::StartFramework("../config/ipcserver.toml");
    if (code != 0)
    {
        return code;
    }
    ::OI::Log::Init();

    if(!IpcServer::GetInstance().Init())
    {
        printf("IpcServer Init error\n");
        return -1;
    }
    if (!IpcServer::GetInstance().StartServer())
    {
        printf("IpcServer StartServer error\n");
        return -1;
    }
    printf("ipc server listen\n");
    fflush(stdout);
    IpcServer::GetInstance().StartHttpProbe();

    return ::OI::Base::RunFramework();
}
