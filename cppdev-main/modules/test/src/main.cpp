#include <stdio.h>
#include "net_header.h"
#include "base_header.h"
#include "testclient.h"
#include "testserver.h"
#include "bytes.h"
#include "poll.h"
#include "session.h"
#include "config.h"
#include "base_timer_manager.h"
#include "test_timer.h"
#include "test_net.h"
#include "test_common_bytes.h"
#include "test_log.h"

int main(int argc, char* argv[])
{
    //加载模块配置
    if (!Config::GlobalMutableConfig().Parse("..//config/test.toml"))
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



    TestNet::DoPerformanceTest();

    TestTimer::DoPerformanceTest();

    TestCommonBytes::DoPerformanceTest();

    TestLog::DoPerformanceTest();
 
    //启动线程池,驱动进程逻辑
    ThreadPool::GetInstance()->StartWork();
    
    //此处是走不到的.
    return 0;
}
        