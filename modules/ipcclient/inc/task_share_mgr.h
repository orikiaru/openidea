#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class TaskShareMgr
{
	DECLARE_SINGLETON(TaskShareMgr)
	TaskShareMgr() = default;
	// 共享结果任务：只应执行一次，按参数返回字符串或抛异常。
	OI::Coro::TaskResult<std::string> SharedResultChild(std::shared_ptr<std::atomic<int>> pRuns, bool bThrow);
	// 无结果的共享任务，只应执行一次。
	OI::Coro::Task SharedVoidChild(std::shared_ptr<std::atomic<int>> pRuns);
	// 同一任务等两轮。完成后再次等待不能重跑，也不能拿到被移空的结果。
	OI::Coro::TaskResult<int> SharedResultWaiter(OI::Coro::TaskResult<std::string> stTask, bool bThrow);
	// 重复 Start 和重复等待都不能让无结果任务再跑一遍。
	OI::Coro::Task SharedVoidStarter(OI::Coro::Task stTask);
	// 单线程下从非 poll 线程 StartTask 被拒绝。多线程不测这条。
	OI::Coro::TaskResult<int> CheckForeignTaskStart();
	// 64 个等待者、拷贝、移动和替换都只执行一次。
	OI::Coro::TaskResult<int> CheckSharedTasks();

  public:
	// 64 个等待者共享一个任务。完成后再次等待不重跑，拷贝、移动和替换都只执行一次。
	OI::Coro::TaskResult<int> Run();
	// 单线程下从非 poll 线程 StartTask 被拒绝。多线程不测这条，直接记过。
	OI::Coro::TaskResult<int> RunForeign();
};
