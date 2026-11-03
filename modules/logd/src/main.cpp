#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "log_server.h"
#include "log_manager.h"

// logd 自己的日志直接进本地文件，不作为日志客户端连自己。
static bool WriteSelfLog(LOG_LEVEL lv, const char* text, size_t len)
{
    ::OI::Base::Bytes body(text, len);
    LogManager::GetInstance().WriteLog(lv, body);
    return true;
}

int main(int argc, char* argv[])
{
    int code = ::OI::Base::StartFramework("../config/logd.toml");
    if (code != 0)
    {
        return code;
    }

    // 写线程起来之前的日志只打 stdout
    if (!LogManager::GetInstance().Init())
    {
        printf("LogManager Init error\n");
        return -3;
    }
    ::OI::Log::InitLocal(WriteSelfLog);

    if (!LogServer::GetInstance().Init())
    {
        LOG_ERROR("LogServer::Init error");
        ::OI::Log::InitLocal(NULL);
        LogManager::GetInstance().Stop();
        return -1;
    }
    if (!LogServer::GetInstance().StartServer())
    {
        LOG_ERROR("LogServer::StartServer error");
        ::OI::Log::InitLocal(NULL);
        LogManager::GetInstance().Stop();
        return -1;
    }

    code = ::OI::Base::RunFramework();

    //网络和工人都停了，不会再进日志。写线程把剩下的写完再退出。
    ::OI::Log::InitLocal(NULL);
    LogManager::GetInstance().Stop();
    return code;
}
