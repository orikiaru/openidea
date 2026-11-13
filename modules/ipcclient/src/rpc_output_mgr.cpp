#include "rpc_output_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "rpc_probe.h"
#include "oi_http.h"
#include "oi_log_client.h"
#include "oi_log.h"
#include "oi_registry.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "oi_rpcdata_jsonall.h"
#include <limits>
#include "oi_config.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "oi_poll.h"
#include "oi_net_session.h"
#include "oi_timer_manager.h"
#include <uv.h>
#include "test_board.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

// 只统计提交，仍走真实连接、序列化、服务端 Hold 和回包。
class OutputRpcProbe : public DeferredRpcProbe
{
	std::atomic<int> m_iSubmissions{0};

  public:
	void SubmitCall(uint64_t iId, const OI::Base::Bytes& stData) override
	{
		m_iSubmissions.fetch_add(1, std::memory_order_relaxed);
		ProtoClientManager::SubmitCall(iId, stData);
	}
	int Submissions() const
	{
		return m_iSubmissions.load(std::memory_order_relaxed);
	}
};

OI::Coro::TaskResult<int> RpcOutputMgr::CheckOutputRpcTask(sid_t iSessionId)
{
	int iBad = 0;
	static OutputRpcProbe stClient;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -1;
	const int iBefore = stClient.Submissions();
	// 普通函数返回后局部请求消失；管理器和输出活到所有等待者完成。
	auto fnCall = [pOutput = &stOutput]()
	{
		OI::RpcData::HoldReq stLocal;
		stLocal.iDelayMilliseconds = 100;
		stLocal.iMark = 731;
		auto stTask = OI::Rpc::RpcCall(stClient, stLocal, *pOutput, OI::Timer::Seconds(3));
		stLocal.iMark = -731;
		return stTask;
	};
	auto stCall = fnCall();
	static_assert(std::is_same_v<decltype(stCall), OI::Coro::TaskResult<int32_t>>);
	std::vector<OI::Coro::TaskResult<int>> vWaiters;
	for (int iIndex = 0; iIndex < 16; ++iIndex)
	{
		vWaiters.push_back(OI::Coro::StartTask([stCall](OI::Coro::Token)
		{
			return RpcOutputWait::Await(stCall);
		}));
	}
	if ((co_await stClient.StartClient()) != OI_OK)
	{
		++iBad;
	}
	for (const auto& stWaiter : vWaiters)
	{
		iBad += co_await stWaiter;
	}
	if (stOutput.iMark != 731)
	{
		++iBad;
	}
	// 任务已完成。修改输出，再从多个任务重复 await，不能重新发送或再次写输出。
	stOutput.iMark = 732;
	vWaiters.clear();
	for (int iIndex = 0; iIndex < 16; ++iIndex)
	{
		vWaiters.push_back(OI::Coro::StartTask([stCall](OI::Coro::Token)
		{
			return RpcOutputWait::Await(stCall);
		}));
	}
	for (const auto& stWaiter : vWaiters)
	{
		iBad += co_await stWaiter;
	}
	const int iSubmitted = stClient.Submissions() - iBefore;
	if (iSubmitted != 1 || stOutput.iMark != 732)
	{
		++iBad;
	}
	LOG_TRACE("hold output rpc shared waiters=16 submissions=%d bad=%d", iSubmitted, iBad);

	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 733;
	auto& stManager = IpcClient::GetInstance();
	auto stDirect = stManager.Call(stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stDirect), OI::Coro::TaskResult<int32_t>>);
	if (co_await stDirect != 0 || stOutput.iMark != 733)
	{
		++iBad;
	}
	stRequest.iMark = 734;
	auto stPinned = OI::Rpc::RpcCall(stManager, iSessionId, stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stPinned), OI::Coro::TaskResult<int32_t>>);
	if (co_await stPinned != 0 || stOutput.iMark != 734)
	{
		++iBad;
	}
	static DeferredRpcProbe stUnconnected;
	const auto stBefore = OI::Rpc::GetRpcWaitStats();
	if (co_await OI::Rpc::RpcCall(stUnconnected, sid_t(12345), stRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_CONNECT ||
	    stOutput.iMark != 734 || OI::Rpc::GetRpcWaitStats().iCalls != stBefore.iCalls)
	{
		++iBad;
	}
	stRequest.iDelayMilliseconds = -1;
	auto stError = stManager.Call(stRequest, stOutput, OI::Timer::Seconds(3));
	if (co_await stError != 1001 || co_await stError != 1001 || stOutput.iMark != 734)
	{
		++iBad;
	}

	stRequest.iDelayMilliseconds = 1000;
	stRequest.iMark = 735;
	auto stTimeout = stManager.Call(stRequest, stOutput, OI::Timer::Milliseconds(100));
	if (co_await stTimeout != OI_ERR_TIMEOUT || stOutput.iMark != 734)
	{
		++iBad;
	}
	// 复用同一输出给下一次成功请求，迟到回包不得覆盖新结果。
	stRequest.iDelayMilliseconds = 0;
	stRequest.iMark = 736;
	if (co_await stManager.Call(stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 736)
	{
		++iBad;
	}
	co_await OI::Coro::yield(OI::Timer::Milliseconds(1200));
	if (stOutput.iMark != 736 || co_await stTimeout != OI_ERR_TIMEOUT || stOutput.iMark != 736)
	{
		++iBad;
	}
	stClient.CloseAllSessions();
	LOG_TRACE("hold output rpc direct pinned error late bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> RpcOutputMgr::CheckDeferredRpcSession()
{
	static DeferredRpcProbe stClient;
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 0;
	stRequest.iMark = 31415;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -1;
	auto stValueWait = OI::Rpc::RpcCall(stClient, stRequest, OI::Timer::Seconds(3));
	auto stOutputWait = OI::Rpc::RpcCall(stClient, stRequest, stOutput, OI::Timer::Seconds(3));
	int iBad = 0;
	int iValueCode = OI_ERR_CONNECT;
	int iOutputCode = OI_ERR_CONNECT;
	int iPinnedCode = 0;
	if ((co_await stClient.StartClient()) != OI_OK)
	{
		++iBad;
		// 输出接口创建即运行，失败路径也要等任务结束再释放输出。
		iOutputCode = co_await stOutputWait;
	}
	else
	{
		auto stValue = co_await stValueWait;
		iValueCode = stValue.GetRet();
		iOutputCode = co_await stOutputWait;
		if (iValueCode != 0 || stValue.GetRes().iMark != 31415 || iOutputCode != 0 || stOutput.iMark != 31415)
		{
			++iBad;
		}
		auto stPinned = co_await OI::Rpc::RpcCall(stClient, INVALID_SID, stRequest, OI::Timer::Seconds(3));
		iPinnedCode = stPinned.GetRet();
		if (iPinnedCode != OI_ERR_CONNECT)
		{
			++iBad;
		}
	}
	stClient.CloseAllSessions();
	LOG_TRACE("hold deferred rpc value=%d output=%d pinned=%d bad=%d", iValueCode, iOutputCode, iPinnedCode, iBad);
	fflush(stdout);
	co_return iBad;
}

// 按值和输出引用同时在飞。没连上时，固定 sid 回 CONNECT。
OI::Coro::TaskResult<int> RpcOutputMgr::RunDeferred()
{
	co_return co_await CheckDeferredRpcSession();
}

// 16 个等待者只提交一次，重复等待不重写输出。固定会话、业务错误、超时后的迟到回包都不覆盖新结果。
OI::Coro::TaskResult<int> RpcOutputMgr::Run(sid_t iSessionId)
{
	co_return co_await CheckOutputRpcTask(iSessionId);
}
