#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class BlockingMgr
{
	DECLARE_SINGLETON(BlockingMgr)
	BlockingMgr() = default;
	// 同一个阻塞任务等两次，两次都成功才算过。
	OI::Coro::TaskResult<int> BlockingWaiter(OI::Coro::TaskResult<int32_t> stTask);
	// 阻塞池按类型隔离。没配的类型拒绝，管道读在指定池里跑。
	OI::Coro::TaskResult<int> CheckBlockingCategories();
	// 128 个后台任务和 64 个等待者只执行一次；管道读期间定时器和 RPC 仍能跑。
	OI::Coro::TaskResult<int> CheckBlockingWork();

  public:
	// 阻塞池按类型隔离。128 个后台任务和 64 个等待者只执行一次；管道读在后台时，前台定时器和 RPC 仍能跑。
	OI::Coro::TaskResult<int> Run();
};
