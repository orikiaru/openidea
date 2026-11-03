#pragma once

namespace OI { namespace Base {
// 读进程配置，再初始化线程池、定时器、IO 复用。成功返回 0。
// 失败时打印原因并返回 main 该用的退出码：配置读不了 -2，框架初始化失败 -1。
int StartFramework(const char* pConfigPathParam);
// 跑任务、定时器和协程，直到 RequestStop 或收到退出信号。线程都 join 完才返回退出码。
int RunFramework();
}}
