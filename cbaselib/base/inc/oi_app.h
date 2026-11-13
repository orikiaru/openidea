#pragma once
#include <cstdint>

namespace OI
{
namespace Base
{
// 读进程配置，再初始化线程池、定时器、IO 复用。成功返回 0。
// 失败时打印阶段和错误码，返回对应的框架错误码。
int32_t StartFramework(const char* pConfigPath);
// 跑任务、定时器和协程，直到 RequestStop 或收到退出信号。线程都 join 完才返回退出码。
int RunFramework();
}
}
