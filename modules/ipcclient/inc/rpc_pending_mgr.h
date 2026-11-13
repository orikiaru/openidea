#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"

class RpcPendingMgr
{
	DECLARE_SINGLETON(RpcPendingMgr)
	RpcPendingMgr() = default;
	struct PendingClient;
	// 在途调用、过期、时间桶和会话索引都回到比较前的数量。
	bool SameWaitStats(const OI::Rpc::RpcWaitStats& stBaseline);
	// 建连前的发送额度连续计数，写出后才恢复。
	int CheckPreconnectSendBudget();
	// 标准回调的可选上下文跨挂起保持有效。
	OI::Coro::TaskResult<int> CheckRpcContext(PendingClient& stPending);
	// 在探测客户端上登记一条待发调用。
	uint64_t BindPendingCall(PendingClient& stPending, int32_t* pResult, int64_t iTimeoutMilliseconds);
	// 待发条数和字节超限回 BUSY，腾出或失败后额度恢复。
	int CheckPendingLimits(PendingClient& stPending);
	// 会话索引换绑后，旧连接断开只能完成仍属于它的调用。
	int CheckRpcSessionIndex(PendingClient& stPending);
	// 跨分片、跨批次断连清理。回调抛异常也要完成后续项并归还 guard。
	int CheckRpcDisconnectDrain(PendingClient& stPending);
	// 登记后序列化异常，以及发送异常与完成回调竞争。恢复后不能留下悬空等待项。
	OI::Coro::TaskResult<int> CheckRpcExceptions(PendingClient& stPending);
	// 队首留一条长超时，再拒绝十万条。索引数量保持不变。
	int CheckRpcExpiry(PendingClient& stPending);
	// 不同剩余时长落到同一小段绝对截止时间，而不是每条一个桶。
	OI::Coro::TaskResult<int> CheckDeadlineBuckets();
	// 只在内部配对入口注入错误来源，并确认断连能唤醒等待。
	int CheckRpcSource(sid_t iSessionId);

  public:
	// 建连前发送额度、回调上下文、待发条数和字节、4096 条会话索引、断连排空、提交异常、过期清理、绝对截止时间归桶，以及错误来源和断连唤醒。
	OI::Coro::TaskResult<int> Run(sid_t iSessionId);
};
