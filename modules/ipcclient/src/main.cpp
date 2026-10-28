#include <stdio.h>
#include "oi_net.h"
#include "oi_base.h"
#include "ipcclient.h"

int main(int argc, char* argv[])
{
    //加载模块配置
    if (!OI::Base::Config::GlobalMutableConfig().Parse("../config/ipcclient.toml"))
	{
		printf("ParseConfigError\n");
		return -2;
	}
 
    //初始化线程池
    OI::Thread::ThreadPool::GetInstance().Init();
    
    //初始化定时器
    BaseTimerManager::GetInstance().Init();
    
    //初始化IO复用
	Poll::GetInstance().Init();

    //初始化日志服务
    Log::Init();
 
    //添加组件逻辑,如网络组件,自定义单例对象等初始化逻辑
    
    if(!IpcClient::GetInstance().Init())
    {
        printf("IpcClient Init error\n");
        return -1;
    }
    IpcClient::GetInstance().StartClient();

 
    //跑任务、定时器和协程。RequestStop 之后 join 完线程再返回，静态对象这时才析构。
    OI::Thread::ThreadPool::GetInstance().StartWork();
    return OI::Thread::ThreadPool::GetInstance().ExitCode();
}
        