#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class NestedResultMgr
{
	DECLARE_SINGLETON(NestedResultMgr)
	NestedResultMgr() = default;
	// 一个工人收齐的完成数、捕获数和错误数。
	struct NestedCounts
	{
		int iCompleted = 0;
		int iCaught = 0;
		int iBad = 0;
	};
	// 按取值决定直接返回、挂起后返回，或抛异常。
	OI::Coro::TaskResult<int> ResultStressChild(int iValue);
	// 等子任务结束，结果加一后交回。
	OI::Coro::TaskResult<int> ResultStressParent(int iValue);
	// 一个工人跑一批嵌套任务，成功和异常都计入 NestedCounts。
	OI::Coro::TaskResult<NestedCounts> NestedResultsWorker(int iWorker);
	// 多个工人并行，完成数和捕获数都要对上。
	OI::Coro::TaskResult<int> CheckNestedResults();

  public:
	// 嵌套 TaskResult：成功和抛异常的子任务都收齐，完成数和捕获数对得上。
	OI::Coro::TaskResult<int> Run();
};
