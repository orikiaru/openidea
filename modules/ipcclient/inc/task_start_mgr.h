#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class TaskStartMgr
{
	DECLARE_SINGLETON(TaskStartMgr)
	TaskStartMgr() = default;
	// 共享任务的执行次数、唤醒次数，以及每个等待者是否只醒一次。
	struct SharedWaitersState
	{
		std::atomic<int> iRuns{0};
		std::atomic<int> iAwakened{0};
		std::atomic<int> iBad{0};
		std::vector<std::atomic<int>> vHits;
		explicit SharedWaitersState(int iCount)
		    : vHits(iCount)
		{
		}
	};
	// 创建后先走到第一次挂起，并记下当时是不是还在 poll 回调里。
	OI::Coro::Task HotStartChild(std::shared_ptr<std::atomic<int>> pStep, std::shared_ptr<std::atomic<bool>> pInCallback = {});
	// 直接创建会先跑一段；poll 回调里创建的任务只入队。
	OI::Coro::TaskResult<int> CheckHotStart();
	// 共享子任务只执行一次，挂起后返回固定值。
	OI::Coro::TaskResult<int> SharedWaitersChild(std::shared_ptr<SharedWaitersState> pState);
	// 等共享任务，每个序号只能被唤醒一次。
	OI::Coro::Task SharedWaiter(OI::Coro::TaskResult<int> stTask, std::shared_ptr<SharedWaitersState> pState, int iIndex);
	// 不同等待者数量下，已启动的共享任务只执行一次。
	OI::Coro::TaskResult<int> CheckStartedTaskWaiters();

  public:
	// 已启动的共享任务只执行一次，每个等待者只被唤醒一次。
	OI::Coro::TaskResult<int> RunWaiters();
	// 创建时先跑到第一次挂起。poll 回调里新建的任务只入队，不在回调里接着跑。
	OI::Coro::TaskResult<int> RunHot();
};
