#include <stdio.h>
#include "net_header.h"
#include "base_header.h"
#include "ipcserver.h"

int main(int argc, char* argv[])
{
    //加载模块配置
    if (!Config::GlobalMutableConfig().Parse("../config/ipcserver.toml"))
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

 
    //单线程在这里跑任务、定时器和协程，没有任务时进入 poll
    ThreadPool::GetInstance()->StartWork();
    
    //此处是走不到的.
    return 0;
}
        