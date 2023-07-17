#include <stdio.h>
#include "net_header.h"
#include "base_header.h"
#include "log_server.h"
#include "log_client.h"
#include "log_manager.h"

int main(int argc, char* argv[])
{
    //加载模块配置
    if (!Config::GlobalMutableConfig().Parse("..//config/logd.toml"))
	{
		printf("ParseConfigError\n");
		return -2;
	}
 
    //初始化线程池
    ThreadPool::GetInstance()->Init();
    
    //初始化定时器
    BaseTimerManager::GetInstance()->Init();
    
    //初始化IO复用
	Poll::GetInstance()->Init();

    //初始化日志服务
    Log::Init();

    //添加组件逻辑,如网络组件,自定义单例对象等初始化逻辑
    if (!LogManager::GetInstance().Init())
    {
        LOG_TRACE("LogManager::Init error");
        return -3;
    }

    if (!LogServer::GetInstance().Init())
    {
        LOG_TRACE("LogServer::Init error");
        return -1;
    }
    if (!LogServer::GetInstance().StartServer())
    {
        LOG_TRACE("LogServer::StartServer error");
        return -1;
    }


    
 
    //启动线程池,驱动进程逻辑
    ThreadPool::GetInstance()->StartWork();
    
    //此处是走不到的.
    return 0;
}
        