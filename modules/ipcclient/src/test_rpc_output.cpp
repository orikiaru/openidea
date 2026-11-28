#include "test_rpc_output.h"
#include "hold_test.h"
#include "oi_rpc_session_manager.h"
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
#include "oi_rpcdata_peernotereq.h"
#include "oi_rpcdata_peerprobereq.h"
#include "oi_rpcdata_peerproberes.h"
#include "oi_thread.h"
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

// 按外部入口的方式收紧：只读自己配置段的 max_receive_buffer_bytes，帧上限跟着减 8。
class ReceiveLimitProbe : public DeferredRpcProbe
{
  public:
	const std::string GetIDString() override
	{
		return "HoldReceiveLimit";
	}
	int32_t LoadLimits()
	{
		return LoadSessionLimits();
	}
};

// 用实际回包编码尺寸构造边界，超限请求的输出不得被覆盖。额度来自显式配置，不是默认值。
OI::Coro::TaskResult<int> TestRpcOutput::CheckReceiveLimit()
{
	static ReceiveLimitProbe stClient;
	int iBad = 0;
	const int32_t iLoad = stClient.LoadLimits();
	const size_t iLimit = stClient.MaxReceiveBufferBytes();
	const size_t iFrameLimit = stClient.MaxFrameBytes();
	LOG_TRACE("hold receive limit configured receive=%zu frame=%zu load=%d", iLimit, iFrameLimit, iLoad);
	if (iLoad != OI_OK || iLimit != 1048576 || iFrameLimit != iLimit - OI::Net::kFrameHeaderBytes)
	{
		co_return 1;
	}
	const int32_t iConnect = co_await stClient.StartClient();
	if (iConnect != OI_OK)
	{
		LOG_TRACE("hold receive limit connect=%d bad=1", iConnect);
		co_return 1;
	}
	OI::RpcData::AllEchoReq stRequest;
	stRequest.stData.sMessage.assign(iLimit - 1024, 'x');
	OI::RpcData::AllEchoRes stExpected;
	stExpected.stData = stRequest.stData;
	stExpected.iCode = 1;
	OI::Base::BytesStream stBody;
	stBody << uint64_t(1) << int32_t(0);
	stExpected.serialize(stBody);
	const size_t iInitialSize = OI::Rpc::EncodePbFrame(OI::Rpc::RpcProtoId<OI::RpcData::AllEchoRes>::iValue, stBody).Size();
	if (iInitialSize >= iLimit)
	{
		LOG_TRACE("hold receive limit initial=%zu bad=1", iInitialSize);
		co_return 1;
	}
	stRequest.stData.sMessage.append(iLimit - iInitialSize, 'x');
	stExpected.stData = stRequest.stData;
	OI::Base::BytesStream stExactBody;
	stExactBody << uint64_t(1) << int32_t(0);
	stExpected.serialize(stExactBody);
	const size_t iExactSize = OI::Rpc::EncodePbFrame(OI::Rpc::RpcProtoId<OI::RpcData::AllEchoRes>::iValue, stExactBody).Size();
	OI::RpcData::AllEchoRes stOutput;
	const int32_t iExact = co_await stClient.Call(stRequest, stOutput, OI::Timer::Seconds(10));
	if (iExactSize != iLimit || iExact != OI_OK || stOutput.stData.sMessage != stRequest.stData.sMessage || stOutput.iCode != 1)
	{
		++iBad;
	}
	stRequest.stData.sMessage.push_back('x');
	stExpected.stData = stRequest.stData;
	OI::Base::BytesStream stOverBody;
	stOverBody << uint64_t(1) << int32_t(0);
	stExpected.serialize(stOverBody);
	const size_t iOverSize = OI::Rpc::EncodePbFrame(OI::Rpc::RpcProtoId<OI::RpcData::AllEchoRes>::iValue, stOverBody).Size();
	stOutput.stData.sMessage = "preserved";
	stOutput.iCode = 713;
	const int32_t iOver = co_await stClient.Call(stRequest, stOutput, OI::Timer::Seconds(10));
	// 回包超过本端接收额度：客户端断开连接，在途调用以连接错误结束，不能等到超时。
	if (iOverSize != iLimit + 1 || iOver != OI_ERR_CONNECT || stOutput.stData.sMessage != "preserved" || stOutput.iCode != 713)
	{
		++iBad;
	}
	LOG_TRACE("hold receive limit exact=%zu exact_code=%d over_code=%d preserved=%d bad=%d", iExactSize, iExact, iOver,
	    stOutput.stData.sMessage == "preserved" && stOutput.iCode == 713, iBad);
	co_return iBad;
}

// 每个 28 字节字符串线上占 32 字节，逻辑计费另加 sizeof(std::string)。帧约 43 MiB，计费落在 (64 MiB, 256 MiB)。
// 固定 64 MiB 预算时服务端解请求就抛 ParserException，调用拿不到 OI_OK。
OI::Coro::TaskResult<int> TestRpcOutput::CheckDecodeBudget()
{
	static DeferredRpcProbe stClient;
	constexpr size_t iWordBytes = 28;
	constexpr size_t iWordCount = 1400000;
	const size_t iFloor = OI::Base::BytesStream::DefaultDecodeBudgetBytes();
	const size_t iScaled = 4 * stClient.MaxFrameBytes();
	const size_t iCharged = iWordCount * (sizeof(std::string) + iWordBytes);
	OI::RpcData::AllEchoReq stRequest;
	stRequest.stData.vWords.reserve(iWordCount);
	for (size_t iIndex = 0; iIndex < iWordCount; ++iIndex)
	{
		std::string sWord = std::to_string(iIndex);
		sWord.resize(iWordBytes, 'w');
		stRequest.stData.vWords.push_back(std::move(sWord));
	}
	OI::Base::BytesStream stBody;
	stBody << uint64_t(1);
	stRequest.serialize(stBody);
	const size_t iFrame = OI::Rpc::EncodePbFrame(OI::Rpc::RpcProtoId<OI::RpcData::AllEchoReq>::iValue, stBody).Size();
	int iBad = 0;
	if (iCharged <= iFloor || iCharged >= iScaled || iFrame < 40 * 1024 * 1024 || iFrame > 48 * 1024 * 1024 ||
	    iFrame > stClient.MaxReceiveBufferBytes())
	{
		++iBad;
	}
	int32_t iCode = OI_ERR_CONNECT;
	if ((co_await stClient.StartClient()) != OI_OK)
	{
		++iBad;
	}
	else
	{
		OI::RpcData::AllEchoRes stOutput;
		stOutput.iCode = 713;
		iCode = co_await stClient.Call(stRequest, stOutput, OI::Timer::Seconds(30));
		if (iCode != OI_OK || stOutput.iCode != 1 || stOutput.stData.vWords != stRequest.stData.vWords)
		{
			++iBad;
		}
	}
	stClient.CloseAllSessions();
	LOG_TRACE("hold decode budget scaled words=%zu charged=%zu frame=%zu code=%d bad=%d", iWordCount, iCharged, iFrame, iCode, iBad);
	fflush(stdout);
	co_return iBad;
}

OI::Coro::TaskResult<int> TestRpcOutput::CheckOutputRpcTask(sid_t iSessionId)
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
		auto stTask = stClient.Call(stLocal, *pOutput, OI::Timer::Seconds(3));
		stLocal.iMark = -731;
		return stTask;
	};
	auto stCall = fnCall();
	static_assert(std::is_same_v<decltype(stCall), OI::Coro::TaskResult<int32_t>>);
	std::vector<OI::Coro::TaskResult<int>> vWaiters;
	for (int iIndex = 0; iIndex < 16; ++iIndex)
	{
		auto fnWaitForCall = [stCall]()
		{
			return RpcOutputWait::Await(stCall);
		};
		auto stWaiter = OI::Coro::StartTask(std::move(fnWaitForCall));
		vWaiters.push_back(std::move(stWaiter));
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
		auto fnWaitForCompletedCall = [stCall]()
		{
			return RpcOutputWait::Await(stCall);
		};
		auto stWaiter = OI::Coro::StartTask(std::move(fnWaitForCompletedCall));
		vWaiters.push_back(std::move(stWaiter));
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
	auto stPinned = stManager.Call(iSessionId, stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stPinned), OI::Coro::TaskResult<int32_t>>);
	if (co_await stPinned != 0 || stOutput.iMark != 734)
	{
		++iBad;
	}
	static DeferredRpcProbe stUnconnected;
	const auto stBefore = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	if (co_await stUnconnected.Call(sid_t(12345), stRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_CONNECT ||
	    stOutput.iMark != 734 || OI::Rpc::RpcCallManager::GetInstance().WaitStats().iCalls != stBefore.iCalls)
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

OI::Coro::TaskResult<int> TestRpcOutput::CheckDeferredRpcSession()
{
	static DeferredRpcProbe stClient;
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 0;
	stRequest.iMark = 31415;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -1;
	auto stValueWait = stClient.Call(stRequest, OI::Timer::Seconds(3));
	auto stOutputWait = stClient.Call(stRequest, stOutput, OI::Timer::Seconds(3));
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
		auto stPinned = co_await stClient.Call(INVALID_SID, stRequest, OI::Timer::Seconds(3));
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

void TestRpcOutput::OnClientTaskError(OI::Coro::Token, std::exception_ptr stError)
{
	try
	{
		std::rethrow_exception(stError);
	}
	catch (const std::exception& stException)
	{
		TestRpcOutput::GetInstance().RememberClientTaskError(stException.what());
		fprintf(stderr, "task failed: %s\n", stException.what());
		fflush(stderr);
	}
	catch (...)
	{
		fprintf(stderr, "task failed: unknown exception\n");
		fflush(stderr);
	}
}

void TestRpcOutput::RememberClientTaskError(const std::string& sText)
{
	::OI::Thread::MutexGuard stLock(m_stErrorLocker);
	m_vTaskErrors.push_back(sText);
}

bool TestRpcOutput::SawClientTaskError(const std::string& sNeedle)
{
	::OI::Thread::MutexGuard stLock(m_stErrorLocker);
	for (const auto& sText : m_vTaskErrors)
	{
		if (sText.find(sNeedle) != std::string::npos)
		{
			return true;
		}
	}
	return false;
}

OI::Coro::TaskResult<int32_t> TestRpcOutput::QueryClientNotes(::OI::Rpc::ProtoClientManager& stClient, int32_t iTag)
{
	::OI::RpcData::PeerProbeReq stRequest;
	stRequest.iTag = iTag;
	stRequest.iNotify = 0;
	::OI::RpcData::PeerProbeRes stResponse;
	stResponse.iNoteCount = -1;
	const int32_t iCode = co_await stClient.Call(stRequest, stResponse, ::OI::Timer::Seconds(3));
	if (iCode != OI_OK)
	{
		co_return iCode;
	}
	co_return stResponse.iNoteCount;
}

OI::Coro::TaskResult<int> TestRpcOutput::CheckClientNotify()
{
	int iBad = 0;
	static DeferredRpcProbe stClient;
	constexpr int32_t iEarlyTag = 7201;
	constexpr int32_t iConnectedTag = 7202;
	constexpr int32_t iReconnectTag = 7203;
	constexpr int64_t iWaitMilliseconds = 5000;
	const bool bNotConnected = !stClient.IsConnect();
	::OI::RpcData::PeerNoteReq stEarly;
	stEarly.iTag = iEarlyTag;
	const int32_t iEarlyCode = stClient.Notify(stEarly);
	int32_t iEarly = -1;
	if ((co_await stClient.StartClient()) != OI_OK)
	{
		++iBad;
	}
	else
	{
		const int64_t iEarlyDeadline = ::OI::Timer::SteadyMilliseconds() + iWaitMilliseconds;
		iEarly = co_await QueryClientNotes(stClient, iEarlyTag);
		while (iEarly != 1 && ::OI::Timer::SteadyMilliseconds() < iEarlyDeadline)
		{
			co_await OI::Coro::yield(20);
			iEarly = co_await QueryClientNotes(stClient, iEarlyTag);
		}
	}
	if (!bNotConnected || iEarlyCode != OI_OK || iEarly != 1)
	{
		++iBad;
	}

	::OI::RpcData::PeerNoteReq stConnected;
	stConnected.iTag = iConnectedTag;
	const int32_t iConnectedCode = stClient.Notify(stConnected);
	const int64_t iConnectedDeadline = ::OI::Timer::SteadyMilliseconds() + iWaitMilliseconds;
	int32_t iConnected = co_await QueryClientNotes(stClient, iConnectedTag);
	while (iConnected != 1 && ::OI::Timer::SteadyMilliseconds() < iConnectedDeadline)
	{
		co_await OI::Coro::yield(20);
		iConnected = co_await QueryClientNotes(stClient, iConnectedTag);
	}
	if (iConnectedCode != OI_OK || iConnected != 1)
	{
		++iBad;
	}

	const sid_t iSid = stClient.ConnectedSid();
	::OI::RpcData::PeerNoteReq stDropped;
	stDropped.iTag = iReconnectTag;
	if (iSid == INVALID_SID)
	{
		++iBad;
	}
	// 只接直连这一段。路由用例稍后会换上自己的处理器，两边的名单不共用。
	OI::Coro::SetTaskErrorHandler(&TestRpcOutput::OnClientTaskError);
	stClient.CloseSession(iSid);
	const int32_t iDroppedCode = stClient.Notify(stDropped);
	const std::string sDroppedNeedle = "RPC notification failed protocol=" +
	                                   std::to_string(::OI::Rpc::RpcProtoId<::OI::RpcData::PeerNoteReq>::iValue) + " code=-3";
	bool bDroppedReported = false;
	bool bReconnected = false;
	const int64_t iReconnectDeadline = ::OI::Timer::SteadyMilliseconds() + iWaitMilliseconds;
	while (::OI::Timer::SteadyMilliseconds() < iReconnectDeadline)
	{
		if (!bDroppedReported && SawClientTaskError(sDroppedNeedle))
		{
			bDroppedReported = true;
		}
		const sid_t iNowSid = stClient.ConnectedSid();
		if (stClient.IsConnect() && iNowSid != INVALID_SID && iNowSid != iSid)
		{
			bReconnected = true;
		}
		if (bDroppedReported && bReconnected)
		{
			break;
		}
		co_await OI::Coro::yield(20);
	}
	int32_t iDroppedCount = -1;
	if (bReconnected)
	{
		iDroppedCount = co_await QueryClientNotes(stClient, iReconnectTag);
	}
	const bool bDropped = iDroppedCode == OI_OK && bDroppedReported && iDroppedCount == 0;
	if (!bDropped)
	{
		++iBad;
	}

	constexpr int32_t iAgainTag = 7204;
	::OI::RpcData::PeerNoteReq stAgain;
	stAgain.iTag = iAgainTag;
	const int32_t iAgainCode = stClient.Notify(stAgain);
	const int64_t iAgainDeadline = ::OI::Timer::SteadyMilliseconds() + iWaitMilliseconds;
	int32_t iAgain = co_await QueryClientNotes(stClient, iAgainTag);
	while (iAgain != 1 && ::OI::Timer::SteadyMilliseconds() < iAgainDeadline)
	{
		co_await OI::Coro::yield(20);
		iAgain = co_await QueryClientNotes(stClient, iAgainTag);
	}
	const bool bAgain = iAgainCode == OI_OK && iAgain == 1;
	if (!bAgain)
	{
		++iBad;
	}
	if (iBad != 0)
	{
		LOG_TRACE("hold client notify detail dropped_code=%d reported=%d count=%d reconnected=%d again=%d", iDroppedCode, bDroppedReported ? 1 : 0,
		    iDroppedCount, bReconnected ? 1 : 0, iAgain);
	}
	LOG_TRACE("hold client notify early=%d connected=%d dropped=%d reconnect=%d bad=%d", iEarly == 1 ? 1 : 0, iConnected == 1 ? 1 : 0, bDropped ? 1 : 0,
	    bAgain ? 1 : 0, iBad);
	fflush(stdout);
	co_return iBad;
}

// 按值和输出引用同时在飞。没连上时，固定 sid 回 CONNECT。
OI::Coro::TaskResult<int> TestRpcOutput::RunDeferred()
{
	co_return co_await CheckDeferredRpcSession();
}

// 16 个等待者只提交一次，重复等待不重写输出。固定会话、业务错误、超时后的迟到回包都不覆盖新结果。直连通知也在这里。
OI::Coro::TaskResult<int> TestRpcOutput::Run(sid_t iSessionId)
{
	int iBad = co_await CheckOutputRpcTask(iSessionId);
	iBad += co_await CheckClientNotify();
	iBad += co_await CheckReceiveLimit();
	iBad += co_await CheckDecodeBudget();
	co_return iBad;
}
