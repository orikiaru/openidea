#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class ScheduleMgr
{
	DECLARE_SINGLETON(ScheduleMgr)
	ScheduleMgr() = default;
	// 64 个协程不停 yield(0) 时，新协程仍能在 1.5 秒内跑到。
	OI::Coro::TaskResult<int> CheckSchedulerFairness();
	// 子任务异常跨挂起点传回父任务；作用域关闭后不再接受任务。
	OI::Coro::TaskResult<int> CheckTaskScope();
	// 带捕获的协程 lambda 在临时闭包离开、挂起恢复后仍能读到捕获。
	OI::Coro::TaskResult<int> CheckCapturedTasks();

  public:
	// 64 个协程不停 yield(0) 时，新协程仍能在 1.5 秒内跑到。
	OI::Coro::TaskResult<int> RunFairness();
	// 子任务异常传到父任务。作用域停止后不再接受新任务。
	OI::Coro::TaskResult<int> RunScope();
	// 带捕获的协程 lambda 挂起恢复后，仍能读到捕获的内容。
	OI::Coro::TaskResult<int> RunCaptured();
};
