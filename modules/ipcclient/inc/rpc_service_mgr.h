#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class RpcServiceMgr
{
	DECLARE_SINGLETON(RpcServiceMgr)
	RpcServiceMgr() = default;
	// 一次命名 Hold 的结局：成功、失败，或并发时被拒绝。
	enum class NamedOutcome
	{
		Success,
		Failed,
		Rejected,
	};
	// 等这一次命名调用，按回包标记归成上面三种结局。
	template <typename T>
	OI::Coro::TaskResult<NamedOutcome> AwaitNamedHold(T stCall);
	// 保留已完成任务再等待时，请求附带的资源全部释放。
	OI::Coro::TaskResult<int> CheckCompletedRpcRequests(sid_t iSessionId);
	// 按服务名和分区的输出引用，8 个等待者只提交一次。
	OI::Coro::TaskResult<int> CheckNamedOutputRpcTask();
	// 全局、任意、分区、批量和多分区调用打到对应服务。并发争用时成功和拒绝的次数对得上。
	OI::Coro::TaskResult<int> CheckNamedServiceRpc();

  public:
	// 全局、任意、分区、批量和多分区调用打到对应服务。并发争用同一个命名调用时，成功和拒绝的次数对得上。
	OI::Coro::TaskResult<int> RunNamed();
	// 按服务名和分区的输出引用，8 个等待者只提交一次。
	OI::Coro::TaskResult<int> RunOutput();
	// 保留已完成任务再等待时，请求附带的资源全部释放。
	OI::Coro::TaskResult<int> RunCompleted(sid_t iSessionId);
};
