#pragma once
#include "oi_lock.h"
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <exception>
#include <string>
#include <vector>

class TestRpcService
{
	DECLARE_SINGLETON(TestRpcService)
	TestRpcService() = default;
	// 一次命名 Hold 的结局：成功、失败，或并发时被拒绝。
	enum class NamedOutcome
	{
		Success,
		Failed,
		Rejected,
	};
	// 任务错误处理器写下的原文。通知失败不回给调用方，从这里看。
	::OI::Thread::Mutex m_stErrorLocker;
	std::vector<std::string> m_vTaskErrors;
	static void OnTaskError(OI::Coro::Token, std::exception_ptr stError);
	void RememberTaskError(const std::string& sText);
	bool SawTaskError(const std::string& sNeedle);
	// 等这一次命名调用。返回码和输出标记一起归成上面三种结局。
	template <typename T, typename Res>
	OI::Coro::TaskResult<NamedOutcome> AwaitNamedHold(T stCall, const Res& stOutput);
	// 保留已完成任务再等待时，请求附带的资源全部释放。
	OI::Coro::TaskResult<int> CheckCompletedRpcRequests(sid_t iSessionId);
	// 按服务名和分区的输出引用，8 个等待者只提交一次。
	OI::Coro::TaskResult<int> CheckNamedOutputRpcTask();
	// 全局、任意、分区、批量和多分区调用打到对应服务。并发争用时成功和拒绝的次数对得上。
	OI::Coro::TaskResult<int> CheckNamedServiceRpc();
	// 问 gameserver 这个分区收到了几条该标记的 PeerNote。失败回错误码。
	OI::Coro::TaskResult<int32_t> QueryZoneNotes(int32_t iZoneId, int32_t iTag);
	// 等到两台分区的计数之和等于期望，或到点。两个分区的计数写回调用方。
	OI::Coro::TaskResult<int32_t> WaitGameserverNotes(int32_t iTag, int32_t iExpected, int64_t iMilliseconds, int32_t& iZone1001, int32_t& iZone1002);
	// 等到任务错误原文里出现这段，或到点。
	OI::Coro::TaskResult<bool> WaitTaskError(const std::string& sNeedle, int64_t iMilliseconds);
	// 路由通知：任选、指定分区、负分区、全部分区、夹一个未登记分区、不存在的服务名，以及断线后立即再发。
	OI::Coro::TaskResult<int> CheckRouteNotify();

  public:
	// 全局、任意、分区、批量和多分区调用打到对应服务。并发争用同一个命名调用时，成功和拒绝的次数对得上。
	OI::Coro::TaskResult<int> RunNamed();
	// 按服务名和分区的输出引用，8 个等待者只提交一次。
	OI::Coro::TaskResult<int> RunOutput();
	// 保留已完成任务再等待时，请求附带的资源全部释放。
	OI::Coro::TaskResult<int> RunCompleted(sid_t iSessionId);
};
