#pragma once
#include <cstdint>

namespace OI
{
namespace Base
{
// 提取 --config <路径>，其他参数保留原顺序；未指定时保留调用方的路径。
int32_t ParseConfigArgument(int& iArgumentCount, char* vArguments[], const char*& pConfigPath);
int32_t StartFramework(const char* pDefaultConfigPath, int& iArgumentCount, char* vArguments[]);
// 读进程配置，再初始化线程池、定时器、IO 复用。成功返回 0。
// 失败时打印阶段和错误码，返回对应的框架错误码。
int32_t StartFramework(const char* pConfigPath);
// 启动失败、不进 RunFramework 就要退出时调用：停掉并 join poll 线程，再从 main 返回。
// 否则 exit 的静态析构会和 poll 线程并发，碰到已经析构的会话和管理器。可重复调用。
void StopFramework();
// 跑任务、定时器和协程，直到 RequestStop 或收到退出信号。线程都 join 完才返回退出码。
int RunFramework();
}
}
