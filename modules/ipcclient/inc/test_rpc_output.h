#pragma once
#include "oi_lock.h"
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <exception>
#include <string>
#include <vector>

namespace OI
{
namespace Rpc
{
class ProtoClientManager;
}
}

class TestRpcOutput
{
	DECLARE_SINGLETON(TestRpcOutput)
	TestRpcOutput() = default;
	// 直连通知失败的原文。和路由那组各记各的，换处理器时不共用一份名单。
	::OI::Thread::Mutex m_stErrorLocker;
	std::vector<std::string> m_vTaskErrors;
	static void OnClientTaskError(OI::Coro::Token, std::exception_ptr stError);
	void RememberClientTaskError(const std::string& sText);
	bool SawClientTaskError(const std::string& sNeedle);
	// 16 个等待者只提交一次。重复等待、固定会话、业务错误和迟到回包都不覆盖输出。
	OI::Coro::TaskResult<int> CheckOutputRpcTask(sid_t iSessionId);
	OI::Coro::TaskResult<int> CheckReceiveLimit();
	// 帧在 64 MiB 以内、逻辑计费超过 64 MiB 的大字符串数组要能往返；预算随帧上限放大。
	OI::Coro::TaskResult<int> CheckDecodeBudget();
	// 按值和输出引用同时在飞。没连上时，固定 sid 回 CONNECT。
	OI::Coro::TaskResult<int> CheckDeferredRpcSession();
	// 问这条直连上的 ipcserver 收到了几条该标记的 PeerNote。失败回错误码。
	OI::Coro::TaskResult<int32_t> QueryClientNotes(::OI::Rpc::ProtoClientManager& stClient, int32_t iTag);
	// 没连上先发、已连上再发。断线后那条不补发，重连后再发才送到。
	OI::Coro::TaskResult<int> CheckClientNotify();

  public:
	// 按值和输出引用同时在飞。没连上时，固定 sid 回 CONNECT。
	OI::Coro::TaskResult<int> RunDeferred();
	// 16 个等待者只提交一次，重复等待不重写输出。固定会话、业务错误、超时后的迟到回包都不覆盖新结果。直连通知也在这里。
	OI::Coro::TaskResult<int> Run(sid_t iSessionId);
};
