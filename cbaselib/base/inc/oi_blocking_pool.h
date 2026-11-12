#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>

namespace OI
{
namespace Thread
{
enum class BlockingType : uint32_t
{
	Default,
	Mysql,
	File,
	Sdk
};
// 常驻管理器构造时调用，保证后台池晚于管理器析构。
void InitializeBlockingPools();
// 在首次提交前配置。每类固定线程数，队列上限不含正在执行的任务。
// 未配置 Default 使用 4 线程/1024 队列；其他类型必须显式配置。
int32_t ConfigureBlockingPool(BlockingType iType, size_t iThreads, size_t iQueueLimit = 1024);
// 包含工作线程及排队额度，用于完成通知尚未消费时的背压。
size_t BlockingPoolCapacity(BlockingType iType);
int32_t SubmitBlockingWork(BlockingType iType, std::function<void()> fnWork);
// 关闭入口并排空已接纳任务，只能在非池工作线程调用。
void StopBlockingPools();
}
}
