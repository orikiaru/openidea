#pragma once
#include "oi_bytes_stream.h"
#include "oi_header.h"
#include "oi_parser.h"
#include "oi_rpc_call.h"
#include "oi_registry.h"
#include "oi_session_manager.h"
#include "oi_rpc_session_manager.h"
#include "oi_lock.h"
#include "oi_duration.h"
#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "oi_coro.h"


namespace OI
{
namespace Rpc
{
// 记录失败阶段和调用关联信息；日志失败不能打断原来的错误回执/收尾。
void ReportRpcError(OI::Coro::Token iToken, uint64_t iCallId, sid_t iSessionId,
    proto_type_t iProtocolId, const char* pStage, std::exception_ptr stError) noexcept;

// 标准服务端协程：co_return 0 成功，正数为业务错误。
using RpcTask = OI::Coro::TaskResult<int32_t>;

// 仅需要连接信息的处理函数接收上下文；生命周期覆盖本次处理协程。
struct RpcContext
{
	// 处理这条请求的任务标识。
	OI::Coro::Token iToken;
	// 请求从哪个管理器进来。
	::OI::Net::SessionManager& stManager;
	// 请求所在的会话。回包发回这里。
	sid_t iSessionId;
};

template <typename T>
struct RpcProtoId;

void BindRpcHandler(proto_type_t iId, std::function<OI::Coro::Task(OI::Coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> fnHandle);

// 请求和回包共用一个协议号。调用号最高位是 1 表示请求，回包清掉这一位。
// 请求：调用号 + Req；回执：调用号 + int32_t retcode + 成功时的 Res。
// 0 成功，负数保留给框架错误，正数用于业务错误。失败不序列化 Res。

// 业务任务外面这一层：解包，co_await 业务任务，再把带调用号的回包写出去。
template <typename Req, typename Res, typename F>
OI::Coro::Task RunHandledRpc(F fnFunction, proto_type_t iResponseId, OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId, ::OI::Base::BytesStream stBody)
{
	uint64_t iCallIdLocal = 0;
	try
	{
		stBody >> iCallIdLocal;
	}
	catch (::OI::Base::ParserException&)
	{
		ReportRpcError(iToken, 0, iSessionId, iResponseId, "decode_call_id", std::current_exception());
		co_return;
	}
	// 单向消息发到了请求处理函数上：照常处理，不回包，免得回执被对端当成又一条单向消息。
	const bool bNotify = iCallIdLocal == 0;
	iCallIdLocal &= ~(uint64_t)RPC_REQ_MASK;
	Req stRequestLocal{};
	Res stResponseLocal{};
	int32_t iRetcode = 0;
	const char* pStage = "decode_request";
	try
	{
		stRequestLocal.deserialize(stBody);
		pStage = "handler";
		RpcContext stContext{iToken, *pManager, iSessionId};
		auto stInvoke = [pFunction = &fnFunction, pRequest = &stRequestLocal, pResponse = &stResponseLocal, pContext = &stContext]()
		{
			if constexpr (std::is_invocable_v<F&, Req&, Res&>)
			{
				return (*pFunction)(*pRequest, *pResponse);
			}
			else
			{
				static_assert(std::is_invocable_v<F&, const RpcContext&, Req&, Res&>, "RPC handler must accept (req, res) or (context, req, res)");
				return (*pFunction)(*pContext, *pRequest, *pResponse);
			}
		};
		using Return = decltype(stInvoke());
		if constexpr (std::is_same_v<Return, int32_t>)
		{
			iRetcode = stInvoke();
		}
		else
		{
			static_assert(std::is_same_v<Return, RpcTask>,
			    "RPC handler must return RpcTask or int32_t");
			iRetcode = co_await OI::Coro::InvokeTask(OI::Coro::TaskLaunch{iToken, false, false}, stInvoke);
		}
	}
	catch (::OI::Base::ParserException&)
	{
		iRetcode = OI_ERR_FAIL;
		ReportRpcError(iToken, iCallIdLocal, iSessionId, iResponseId, pStage, std::current_exception());
	}
	catch (...)
	{
		iRetcode = OI_ERR_FAIL;
		ReportRpcError(iToken, iCallIdLocal, iSessionId, iResponseId, pStage, std::current_exception());
		OI::Coro::ReportTaskError(iToken, std::current_exception());
	}
	if (bNotify)
	{
		co_return;
	}
	::OI::Base::BytesStream stFrame;
	try
	{
		::OI::Base::BytesStream stOutput;
		stOutput << iCallIdLocal << iRetcode;
		if (iRetcode == 0)
		{
			stResponseLocal.serialize(stOutput);
		}
		stFrame = EncodePbFrame(iResponseId, stOutput);
	}
	catch (...)
	{
		ReportRpcError(iToken, iCallIdLocal, iSessionId, iResponseId, "encode_response", std::current_exception());
		OI::Coro::ReportTaskError(iToken, std::current_exception());
		// 丢弃已经写入的半包，重新构造只有调用号和错误码的回执。
		try
		{
			::OI::Base::BytesStream stFailure;
			stFailure << iCallIdLocal << int32_t(OI_ERR_FAIL);
			stFrame = EncodePbFrame(iResponseId, stFailure);
		}
		catch (...)
		{
			ReportRpcError(iToken, iCallIdLocal, iSessionId, iResponseId, "encode_failure_reply", std::current_exception());
			OI::Coro::ReportTaskError(iToken, std::current_exception());
			pManager->CloseSession(iSessionId);
			co_return;
		}
	}
	try
	{
		if (!pManager->SendBytes(iSessionId, stFrame.GetMutableBuff()))
		{
			pManager->CloseSession(iSessionId);
		}
	}
	catch (...)
	{
		ReportRpcError(iToken, iCallIdLocal, iSessionId, iResponseId, "send_response", std::current_exception());
		OI::Coro::ReportTaskError(iToken, std::current_exception());
		pManager->CloseSession(iSessionId);
	}
	co_return;
}

template <typename Req, typename Res, typename F>
void HandleRpc(F fnFunction)
{
	const proto_type_t iResponseIdLocal = RpcProtoId<Res>::iValue;
	BindRpcHandler(RpcProtoId<Req>::iValue, [fnFunction, iResponseIdLocal](OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId, ::OI::Base::BytesStream stBody) -> OI::Coro::Task
	{
		return RunHandledRpc<Req, Res>(fnFunction, iResponseIdLocal, iToken, pManager, iSessionId, std::move(stBody));
	});
}

// 请求类型已关联响应，无需在注册处重复指定。
template <typename Req, typename F>
void HandleRpc(F fnFunction)
{
	HandleRpc<Req, typename RpcProtoId<Req>::Response>(std::move(fnFunction));
}

// 单向消息：调用号 0 + Msg，不回包。处理函数接收 (msg) 或 (context, msg)，返回 RpcTask 或 int32_t，返回码没人收，只是和 RPC 写法一致。
template <typename Msg, typename F>
OI::Coro::Task RunHandledNotify(F fnFunction, OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId, ::OI::Base::BytesStream stBody)
{
	uint64_t iCallIdLocal = 0;
	Msg stMessageLocal{};
	const char* pStage = "decode_notify";
	try
	{
		stBody >> iCallIdLocal;
		stMessageLocal.deserialize(stBody);
		pStage = "notify_handler";
		RpcContext stContext{iToken, *pManager, iSessionId};
		auto stInvoke = [pFunction = &fnFunction, pMessage = &stMessageLocal, pContext = &stContext]()
		{
			if constexpr (std::is_invocable_v<F&, Msg&>)
			{
				return (*pFunction)(*pMessage);
			}
			else
			{
				static_assert(std::is_invocable_v<F&, const RpcContext&, Msg&>, "notify handler must accept (msg) or (context, msg)");
				return (*pFunction)(*pContext, *pMessage);
			}
		};
		using Return = decltype(stInvoke());
		if constexpr (std::is_same_v<Return, int32_t>)
		{
			stInvoke();
		}
		else
		{
			static_assert(std::is_same_v<Return, RpcTask>, "notify handler must return RpcTask or int32_t");
			co_await OI::Coro::InvokeTask(OI::Coro::TaskLaunch{iToken, false, false}, stInvoke);
		}
	}
	catch (::OI::Base::ParserException&)
	{
		ReportRpcError(iToken, iCallIdLocal, iSessionId, RpcProtoId<Msg>::iValue, pStage, std::current_exception());
	}
	catch (...)
	{
		ReportRpcError(iToken, iCallIdLocal, iSessionId, RpcProtoId<Msg>::iValue, pStage, std::current_exception());
		OI::Coro::ReportTaskError(iToken, std::current_exception());
	}
	co_return;
}

template <typename Msg, typename F>
void HandleNotify(F fnFunction)
{
	BindRpcHandler(RpcProtoId<Msg>::iValue, [fnFunction](OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId, ::OI::Base::BytesStream stBody) -> OI::Coro::Task
	{
		return RunHandledNotify<Msg>(fnFunction, iToken, pManager, iSessionId, std::move(stBody));
	});
}

// 发一条单向消息，不等回包。返回 SendBytesResult 的错误码，编码失败是 OI_ERR_ENCODE。
template <typename Msg>
int32_t RpcNotify(::OI::Net::SessionManager& stManager, sid_t iSessionId, const Msg& stMessage)
{
	::OI::Base::BytesStream stFrame;
	try
	{
		::OI::Base::BytesStream stBody;
		stBody << uint64_t(0);
		stMessage.serialize(stBody);
		stFrame = EncodePbFrame(RpcProtoId<Msg>::iValue, stBody);
	}
	catch (...)
	{
		return OI_ERR_ENCODE;
	}
	return stManager.SendBytesResult(iSessionId, stFrame.GetMutableBuff());
}

// 返回调用号。回包、超时或 WakeRpcCall 三者只有一个会触发 decode/resume。
// 回包回调携带实际匹配的 sid，日志不得用可能已重连的当前会话替代来源。
uint64_t BindRpcCall(::OI::Net::SessionManager* pManager, sid_t iSessionId, proto_type_t iResponseId, std::coroutine_handle<> stHandle, int32_t* pResult, int64_t iTimeoutMilliseconds, std::function<bool(::OI::Base::BytesStream&, sid_t)> fnDecode, std::function<void()> fnResume, std::shared_ptr<std::atomic<int32_t>> pGuard = {}, RpcSender* pSender = nullptr);
void FailRpcCalls(::OI::Net::SessionManager* pManager, sid_t iSessionId);
// 取走等待项且不调用完成回调。已经在回调里的，等 WaitRpcGuard 归零。
void AbandonRpcCall(uint64_t iCallId);
void WaitRpcGuard(const std::shared_ptr<std::atomic<int32_t>>& pGuard);
bool WakeRpcCall(uint64_t iCallId, int32_t iCode, ::OI::Base::BytesStream* pBody);

// 发出请求后挂起。回包或超时回调里 Schedule 回来。回包可能比挂起先到。
template <typename Req, typename Res>
struct [[nodiscard]] RpcCallAwaiter
{
	static_assert(RpcProtoId<Req>::iValue == RpcProtoId<Res>::iValue, "RPC request/response protocol mismatch");
	::OI::Net::SessionManager* pManager;
	RpcSender* pClient = nullptr; // 便捷调用在提交时选择会话，显式 sid 调用仍固定会话。
	sid_t iSessionId;
	Req* pRequest;
	Res* pResponse;
	int32_t iResult; // 状态码。exchange(Ready) 之前写完。
	int64_t iTimeoutMilliseconds;
	std::coroutine_handle<> stHandle; // 调用方句柄。门已经是 Waiting 才 Schedule。
	// 完成门。Idle 已发出还没人认领，Ready 结果已写好，Waiting 调用方已挂起。
	std::atomic<OI::Coro::Gate> iGate;
	std::atomic<bool> bAwaitStarted{false}; // 一次性闩。第二次 await 或移动已提交对象直接抛，不进完成门。
	bool bDispatched = false;               // Dispatch 已经 BindRpcCall。析构时槽还在就摘掉。
	// 回包原样留到 await_resume 再解，解包跑在工人上，不占 IO 线程。
	::OI::Base::BytesStream stReply;
	bool bReplied; // 真表示 stReply 里有回包，await_resume 再解。
	sid_t iReplySessionId = INVALID_SID;
	OI::Coro::Token iAwaitToken = 0;
	std::exception_ptr stSubmissionError;
	uint64_t iCallId = 0;
	std::shared_ptr<std::atomic<int32_t>> pGuard;

	RpcCallAwaiter()
	    : pManager(NULL), iSessionId(0), pRequest(NULL), pResponse(NULL), iResult(OI_ERR_FAIL), iTimeoutMilliseconds(180000), stHandle(), iGate(OI::Coro::Gate::Idle), bReplied(false)
	{
	}
	RpcCallAwaiter(const RpcCallAwaiter&) = delete;
	RpcCallAwaiter(RpcCallAwaiter&& stOther)
	    : pManager(ClaimMove(stOther)), pClient(stOther.pClient), iSessionId(stOther.iSessionId), pRequest(stOther.pRequest), pResponse(stOther.pResponse), iResult(stOther.iResult),
	      iTimeoutMilliseconds(stOther.iTimeoutMilliseconds), stHandle(stOther.stHandle), iGate(stOther.iGate.load(std::memory_order_relaxed)),
	      bReplied(false), stSubmissionError(std::move(stOther.stSubmissionError)), iCallId(stOther.iCallId), pGuard(std::move(stOther.pGuard))
	{
		stOther.stHandle = nullptr;
		stOther.iCallId = 0;
		stOther.bDispatched = false;
	}
	~RpcCallAwaiter()
	{
		if (!bDispatched || iCallId == 0)
		{
			return;
		}
		// 槽还在就摘掉，回调不会再进来。回调已经在跑就等它结束，避免写进释放后的等待器。
		AbandonRpcCall(iCallId);
		WaitRpcGuard(pGuard);
	}
	static ::OI::Net::SessionManager* ClaimMove(RpcCallAwaiter& stOther)
	{
		stOther.ClaimAwait();
		if (stOther.bDispatched)
		{
			throw std::logic_error("RpcCall can only be awaited once; cannot reuse or move a submitted call");
		}
		return stOther.pManager;
	}
	void ClaimAwait()
	{
		if (bAwaitStarted.exchange(true, std::memory_order_acq_rel))
		{
			throw std::logic_error("RpcCall can only be awaited once; cannot reuse or move a submitted call");
		}
	}

	bool await_ready() const noexcept
	{
		return false;
	}
	template <typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandle)
	{
		ClaimAwait();
		return Suspend(stHandle);
	}
	// 只负责发出。完成回调在门还没挂起时不调度，调用方可以先发再等。
	void Dispatch()
	{
		if (bDispatched)
		{
			return;
		}
		bDispatched = true;
		iResult = OI_ERR_FAIL;
		bReplied = false;
		stSubmissionError = nullptr;
		iGate.store(OI::Coro::Gate::Idle, std::memory_order_relaxed);
		iCallId = 0;
		pGuard = std::make_shared<std::atomic<int32_t>>(0);
		uint64_t iCallIdLocal = BindRpcCall(pManager, iSessionId, RpcProtoId<Res>::iValue, stHandle, &iResult, iTimeoutMilliseconds, [this](::OI::Base::BytesStream& stInput, sid_t iSourceSession)
		{
			iReplySessionId = iSourceSession;
			stReply = std::move(stInput);
			bReplied = true;
			return true;
		}, [this]()
		{
			// Idle→Ready：结果先写好，调用方还在 Arm 之前，留给 Arm 发现后不挂起。
			// Waiting→Ready：调用方已经挂起，必须由这里 Schedule。
			OI::Coro::Gate iPrevious = iGate.exchange(OI::Coro::Gate::Ready, std::memory_order_acq_rel);
			if (iPrevious == OI::Coro::Gate::Waiting)
			{
				OI::Coro::Schedule(stHandle);
			}
		}, pGuard, pClient);
		iCallId = iCallIdLocal;
		try
		{
			::OI::Base::BytesStream stBody;
			stBody << (iCallIdLocal | (uint64_t)RPC_REQ_MASK);
			pRequest->serialize(stBody);
			::OI::Base::BytesStream stFrame = EncodePbFrame(RpcProtoId<Req>::iValue, stBody);
			if (pClient)
			{
				pClient->SubmitCall(iCallIdLocal, stFrame.GetMutableBuff());
			}
			else
			{
				const int32_t iRet = pManager->SendBytesResult(iSessionId, stFrame.GetMutableBuff());
				if (iRet != OI_OK)
				{
					WakeRpcCall(iCallIdLocal, iRet, NULL);
				}
			}
		}
		catch (...)
		{
			// 不直接从 await_suspend 抛出：超时/断连可能已取走等待项，正在使用 this。
			// 先完成等待，再由同一 gate 确认回调已结束，最后在 await_resume 重抛。
			stSubmissionError = std::current_exception();
			WakeRpcCall(iCallIdLocal, OI_ERR_FAIL, nullptr);
		}
	}
	template <typename Promise>
	bool Arm(std::coroutine_handle<Promise> stHandleLocal)
	{
		if constexpr (std::is_base_of_v<OI::Coro::TaskState, Promise>)
		{
			OI::Thread::MutexGuard stLock(stHandleLocal.promise().stMutex);
			iAwaitToken = stHandleLocal.promise().iTaskId;
		}
		stHandle = stHandleLocal;
		// Idle→Waiting：还在等，挂起。Ready→Waiting：结果已经写完，不挂起，调用方马上 await_resume。
		OI::Coro::Gate iPrevious = iGate.exchange(OI::Coro::Gate::Waiting, std::memory_order_acq_rel);
		return iPrevious != OI::Coro::Gate::Ready;
	}
	template <typename Promise>
	bool Suspend(std::coroutine_handle<Promise> stHandle)
	{
		Dispatch();
		return Arm(stHandle);
	}
	int32_t await_resume()
	{
		if (stSubmissionError)
		{
			std::rethrow_exception(stSubmissionError);
		}
		if (bReplied)
		{
			try
			{
				stReply >> iResult;
				if (iResult == 0)
				{
					Res stDecoded{};
					stDecoded.deserialize(stReply);
					*pResponse = std::move(stDecoded);
				}
			}
			catch (::OI::Base::ParserException&)
			{
				iResult = OI_ERR_FAIL;
				ReportRpcError(iAwaitToken, iCallId, iReplySessionId, RpcProtoId<Res>::iValue, "decode_response", std::current_exception());
			}
		}
		return iResult;
	}
};

template <typename Req, typename Res>
RpcCallAwaiter<Req, Res> MakeRpcCallAwaiter(::OI::Net::SessionManager& stManager, sid_t iSessionId, Req& stRequest, Res& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	RpcCallAwaiter<Req, Res> stAwaiter;
	stAwaiter.pManager = &stManager;
	stAwaiter.iSessionId = iSessionId;
	stAwaiter.pRequest = &stRequest;
	stAwaiter.pResponse = &stResponse;
	stAwaiter.iTimeoutMilliseconds = stTimeout.iMilliseconds;
	return stAwaiter;
}

template <typename Res>
class [[nodiscard]] RpcResult
{
	int32_t m_iReturnCode;
	Res m_stResponse;
	void Check() const
	{
		if (m_iReturnCode != 0)
		{
			throw std::logic_error("RPC failed: check GetRet() before GetRes()");
		}
	}

  public:
	RpcResult(int32_t iReturnCode, Res stResponse)
	    : m_iReturnCode(iReturnCode), m_stResponse(std::move(stResponse))
	{
	}
	int32_t GetRet() const noexcept
	{
		return m_iReturnCode;
	}
	explicit operator bool() const noexcept
	{
		return m_iReturnCode == 0;
	}
	Res& GetRes() &
	{
		Check();
		return m_stResponse;
	}
	const Res& GetRes() const&
	{
		Check();
		return m_stResponse;
	}
	Res GetRes() &&
	{
		Check();
		return std::move(m_stResponse);
	}
	Res GetRes() const&&
	{
		Check();
		return m_stResponse;
	}
};

// 底层单次等待器供框架和故障注入使用；应用使用下面拥有请求的 Task 接口。
template <typename Req>
auto MakeRpcCallAwaiter(RpcSender& stManager, Req& stRequest, typename RpcProtoId<Req>::Response& stResponse,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	auto stCall = MakeRpcCallAwaiter(stManager.Manager(), INVALID_SID, stRequest, stResponse, stTimeout);
	stCall.pClient = &stManager;
	return stCall;
}

// 参数的存储随帧保留；完成时显式销毁请求，不让共享的已完成任务保留大包。
template <typename Req>
struct RpcRequestRelease
{
	std::optional<Req>& stRequest;
	~RpcRequestRelease()
	{
		stRequest.reset();
	}
};

template <typename Req>
OI::Coro::TaskResult<int32_t> RunOutputCall(::OI::Net::SessionManager& stManager, sid_t iSessionId,
    RpcSender* pClient, std::optional<Req> stRequest,
    typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout)
{
	RpcRequestRelease<Req> stRelease{stRequest};
	auto stCall = MakeRpcCallAwaiter(stManager, iSessionId, *stRequest, stResponse, stTimeout);
	stCall.pClient = pClient;
	co_return co_await stCall;
}

// 普通函数在返回前把请求移入任务；响应必须活到任务结束。
template <typename Req>
[[nodiscard]] OI::Coro::TaskResult<int32_t> RpcCall(::OI::Net::SessionManager& stManager, sid_t iSessionId,
    Req stRequest, typename RpcProtoId<Req>::Response& stResponse,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return RunOutputCall<Req>(stManager, iSessionId, nullptr,
	    std::optional<Req>(std::in_place, std::move(stRequest)), stResponse, stTimeout);
}

template <typename Req>
[[nodiscard]] OI::Coro::TaskResult<int32_t> RpcCall(RpcSender& stManager, Req stRequest,
    typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return RunOutputCall<Req>(stManager.Manager(), INVALID_SID, &stManager,
	    std::optional<Req>(std::in_place, std::move(stRequest)), stResponse, stTimeout);
}

template <typename Req, typename Res>
struct [[nodiscard]] RpcValueAwaiter : RpcCallAwaiter<Req, Res>
{
	Req stRequest;
	Res stResponse{};
	RpcValueAwaiter(::OI::Net::SessionManager& stManager, sid_t iSessionId, Req stRequest, ::OI::Timer::MS stTimeout, RpcSender* pClient = nullptr)
	    : stRequest(std::move(stRequest))
	{
		this->pManager = &stManager;
		this->pClient = pClient;
		this->iSessionId = iSessionId;
		this->iTimeoutMilliseconds = stTimeout.iMilliseconds;
	}
	void Submit()
	{
		this->pRequest = &stRequest;
		this->pResponse = &stResponse;
		this->Dispatch();
	}
	template <typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandle)
	{
		// 先独占等待权，再决定要不要发送。并发等待必须在改完成门之前被拒绝。
		this->ClaimAwait();
		if (!this->bDispatched)
		{
			this->pRequest = &stRequest;
			this->pResponse = &stResponse;
			this->Dispatch();
		}
		return this->Arm(stHandle);
	}
	RpcResult<Res> await_resume()
	{
		// 解包会写入 stResponse，必须完成后再移动响应，明确跨编译器求值顺序。
		const int32_t iCode = RpcCallAwaiter<Req, Res>::await_resume();
		return {iCode, std::move(stResponse)};
	}
};

template <typename Res, typename Req>
RpcValueAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& stManager, sid_t iSessionId, Req stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return RpcValueAwaiter<Req, Res>(stManager, iSessionId, std::move(stRequest), stTimeout);
}
// 自动配对响应类型。这个等待对象是完成门，只能等一次。
// 要让两个协程等同一次调用，用返回 TaskResult<int32_t> 的输出引用接口：任务按上面的等待链表共享，响应对象只写一次。
template <typename Req>
auto RpcCall(::OI::Net::SessionManager& stManager, sid_t iSessionId, Req stRequest,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
    -> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {stManager, iSessionId, std::move(stRequest), stTimeout};
}

template <typename Req>
auto RpcCall(RpcSender& stManager, Req stRequest,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
    -> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {stManager.Manager(), INVALID_SID, std::move(stRequest), stTimeout, &stManager};
}

// 常驻应用管理器用成员发送目标适配，不需要继承发送接口。
template <typename Req>
auto MakeRpcCallAwaiter(ProtoClientManager& stManager, Req& stRequest, typename RpcProtoId<Req>::Response& stResponse,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return MakeRpcCallAwaiter(stManager.Sender(), stRequest, stResponse, stTimeout);
}

template <typename Req>
[[nodiscard]] auto RpcCall(ProtoClientManager& stManager, Req stRequest, typename RpcProtoId<Req>::Response& stResponse,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return RpcCall(stManager.Sender(), std::move(stRequest), stResponse, stTimeout);
}

template <typename Req>
[[nodiscard]] auto RpcCall(ProtoClientManager& stManager, Req stRequest,
    ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return RpcCall(stManager.Sender(), std::move(stRequest), stTimeout);
}

// 同一调用的查询、建连和回包共享单调时钟预算。非正值保持无限 RPC 等待语义。
struct RpcDeadline
{
	int64_t iDeadlineMilliseconds = 0;
	RpcDeadline() = default;
	RpcDeadline(::OI::Timer::MS stTimeout)
	{
		if (stTimeout.iMilliseconds > 0)
		{
			const int64_t iNow = ::OI::Timer::SteadyMilliseconds();
			iDeadlineMilliseconds = stTimeout.iMilliseconds > INT64_MAX - iNow ? INT64_MAX : iNow + stTimeout.iMilliseconds;
		}
	}
	template <typename Duration, std::enable_if_t<std::is_convertible_v<Duration, ::OI::Timer::MS>, int> = 0>
	RpcDeadline(Duration stTimeout)
	    : RpcDeadline(::OI::Timer::MS(stTimeout))
	{
	}
	bool Expired() const
	{
		return iDeadlineMilliseconds > 0 && ::OI::Timer::SteadyMilliseconds() >= iDeadlineMilliseconds;
	}
	::OI::Timer::MS Remaining() const
	{
		if (iDeadlineMilliseconds == 0)
		{
			return ::OI::Timer::MS(0);
		}
		return ::OI::Timer::MS((std::max)(int64_t(1), iDeadlineMilliseconds - ::OI::Timer::SteadyMilliseconds()));
	}
};

enum class ServiceRouteStatus
{
	Ok,
	BadName,
	WrongKind,
	Offline,
	Timeout,
	Busy,
};

struct ServiceRoute
{
	ServiceRouteStatus iStatus = ServiceRouteStatus::Offline;
	std::vector<RpcSender*> vClients;
	std::vector<std::shared_ptr<RpcSender>> vOwners;
};

// 路由连接的诊断快照；管理器固定，下面的限制按连接/会话保存。
struct RouteConnectionInfo
{
	std::string sIp;
	std::string sPort;
	sid_t iSessionId = INVALID_SID;
	size_t iSendBytes = 0;
	size_t iReceiveBytes = 0;
	size_t iFrameBytes = 0;
	uint64_t iIncompleteMilliseconds = 0;
	std::pair<size_t, size_t> stPendingLimits;
};
RouteConnectionInfo GetRouteConnectionInfo(const RpcSender& stSender);
std::shared_ptr<void> GetRouteConnectionOwner(const RpcSender& stSender);

inline int32_t RouteError(ServiceRouteStatus iStatus)
{
	switch (iStatus)
	{
	case ServiceRouteStatus::Timeout:
		return OI_ERR_TIMEOUT;
	case ServiceRouteStatus::Busy:
		return OI_ERR_BUSY;
	case ServiceRouteStatus::BadName:
	case ServiceRouteStatus::WrongKind:
		return OI_ERR_FAIL;
	default:
		return OI_ERR_CONNECT;
	}
}

struct RoutePoolStats
{
	size_t iClients = 0;
	size_t iMaxClients = 0;
	size_t iLeasedClients = 0;
	size_t iEvicted = 0;
	int64_t iIdleMilliseconds = 0;
};
RoutePoolStats GetRoutePoolStats();

enum class ServicePick
{
	Any,
	Zone,
	All,
};

// pManager 是发起调用的管理器。不带分区且它自己就在看这个名字时，直接用它，不再另连。
// 返回 true 表示不用再问注册表，结果已经写进 stRoute。
bool PrepareRoute(::OI::Net::SessionManager* pManager, const std::string& sName, ServicePick iPick, const std::string& sZone, ServiceRoute& stRoute);
bool PrepareZones(::OI::Net::SessionManager* pManager, const std::string& sName, const std::vector<int32_t>& vZoneIds, ServiceRoute& stRoute, std::vector<std::string>& vZones);
// bBatch 为真时只接受 name.*，否则只接受 name 或 name.分区。
// 返回 true 表示目标字符串非法，结果在 stRoute。否则写出要拉取的名字、挑选方式和分区。
bool PrepareTarget(const std::string& sTarget, bool bBatch, ServiceRoute& stRoute, std::string& sName, ServicePick& iPick, std::string& sZone);
// 拉取不占线程。fnDone 只调用一次，里面不能再等注册表。
void PullRoute(const std::string& sName, std::function<void(int32_t, const std::vector<::OI::Base::ServiceInstance>&)> fnDone, int64_t iTimeoutMilliseconds = 1000);
// 名单完成后的路由只引用固定地址的池连接；不读取或复用可能切换地址的订阅管理器。
ServiceRoute FinishRoute(const std::string& sName, ServicePick iPick, const std::string& sZone, int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList, ::OI::Net::SessionManager* pManager = nullptr);
// vClients 与分区号等长，空指针表示这个分区当前没登记。
ServiceRoute FinishZones(const std::string& sName, const std::vector<std::string>& vZones, int32_t iCode, const std::vector<::OI::Base::ServiceInstance>& vList, ::OI::Net::SessionManager* pManager = nullptr);

// 挂起等注册表，不在调用线程里同步等。快路径（名字非法、自己已经连着）不挂起。
struct RouteAwaiter
{
	enum class Kind
	{
		Named,
		Zones,
		Target,
	};
	RpcDeadline stDeadline;
	Kind iKind = Kind::Named;
	::OI::Net::SessionManager* pManager = nullptr;
	std::string sName;
	std::string sZone;
	ServicePick iPick = ServicePick::Any;
	std::vector<int32_t> vZones;
	std::string sTarget;
	bool bBatch = false;
	bool bResolved = false;
	int32_t iCode = OI_ERR_CONNECT; // 注册表回包码。exchange(Ready) 之前写完。
	std::vector<::OI::Base::ServiceInstance> vList; // 回包名单。和 iCode 一起在认领完成前写完。
	std::vector<std::string> vZoneNames;
	ServiceRoute stRoute;
	std::coroutine_handle<> stHandle{}; // 调用方句柄。门已经是 Waiting 才 Schedule。
	// 完成门。Idle 还在等拉取，Ready 回包已写入，Waiting 调用方已挂起。
	std::atomic<OI::Coro::Gate> iGate{OI::Coro::Gate::Idle};

	bool await_ready() const noexcept
	{
		return false;
	}
	template <typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandleLocal)
	{
		stHandle = stHandleLocal;
		if (iKind == Kind::Target)
		{
			if (PrepareTarget(sTarget, bBatch, stRoute, sName, iPick, sZone))
			{
				bResolved = true;
				return false;
			}
		}
		if (iKind == Kind::Zones)
		{
			if (PrepareZones(pManager, sName, vZones, stRoute, vZoneNames))
			{
				bResolved = true;
				return false;
			}
		}
		else if (PrepareRoute(pManager, sName, iPick, sZone, stRoute))
		{
			bResolved = true;
			return false;
		}
		if (stDeadline.Expired())
		{
			stRoute.iStatus = ServiceRouteStatus::Timeout;
			bResolved = true;
			return false;
		}
		const int64_t iRemaining = stDeadline.Remaining().iMilliseconds;
		PullRoute(sName, [this](int32_t iCodeLocal, const std::vector<::OI::Base::ServiceInstance>& vListLocal)
		{
			iCode = iCodeLocal;
			try
			{
				vList = vListLocal;
			}
			catch (...)
			{
				iCode = OI_ERR_FAIL;
			}
			// Waiting→Ready 才调度。回包先到时门已是 Ready，下面的 exchange(Waiting) 会选择不挂起。
			if (iGate.exchange(OI::Coro::Gate::Ready, std::memory_order_acq_rel) == OI::Coro::Gate::Waiting)
			{
				OI::Coro::Schedule(stHandle);
			}
		}, iRemaining > 0 ? (std::min)(iRemaining, int64_t(1000)) : 1000);
		// 看见 Ready 表示回包已经写完，不挂起。
		return iGate.exchange(OI::Coro::Gate::Waiting, std::memory_order_acq_rel) != OI::Coro::Gate::Ready;
	}
	ServiceRoute await_resume()
	{
		if (stDeadline.Expired() || iCode == OI_ERR_TIMEOUT)
		{
			stRoute.iStatus = ServiceRouteStatus::Timeout;
			return std::move(stRoute);
		}
		if (iCode == OI_ERR_BUSY)
		{
			stRoute.iStatus = ServiceRouteStatus::Busy;
			return std::move(stRoute);
		}
		if (bResolved)
		{
			return std::move(stRoute);
		}
		if (iKind == Kind::Zones)
		{
			return FinishZones(sName, vZoneNames, iCode, vList, pManager);
		}
		return FinishRoute(sName, iPick, sZone, iCode, vList, pManager);
	}
};

}
}

namespace OI
{
namespace Rpc
{

template <typename Res, typename Req>
OI::Coro::TaskResult<RpcResult<Res>> FailNamed(int32_t iCode)
{
	co_return RpcResult<Res>(iCode, Res{});
}

template <typename Res, typename Req>
OI::Coro::TaskResult<RpcResult<Res>> CallNamed(::OI::Net::SessionManager* pManager, std::string sName, ServicePick iPick, std::string sZone, Req stRequest, RpcDeadline stDeadline)
{
	RouteAwaiter stWait;
	stWait.stDeadline = stDeadline;
	stWait.iKind = RouteAwaiter::Kind::Named;
	stWait.pManager = pManager;
	stWait.sName = std::move(sName);
	stWait.iPick = iPick;
	stWait.sZone = std::move(sZone);
	ServiceRoute stRoute = co_await stWait;
	if (stRoute.iStatus != ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
	{
		int32_t iCode = RouteError(stRoute.iStatus);
		co_return RpcResult<Res>(iCode, Res{});
	}
	if (stDeadline.Expired())
	{
		co_return RpcResult<Res>(OI_ERR_TIMEOUT, Res{});
	}
	auto stCall = RpcCall(*stRoute.vClients[0], std::move(stRequest), stDeadline.Remaining());
	co_return co_await stCall;
}

template <typename Res, typename Req>
auto ProtoClientManager::CallAny(Req stRequest, ::OI::Timer::MS stTimeout)
{
	static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
	return CallNamed<Res>(this, sServiceName, ServicePick::Any, std::string(), std::move(stRequest), stTimeout);
}

template <typename Res, typename Req>
auto ProtoClientManager::Call(int32_t iZoneId, Req stRequest, ::OI::Timer::MS stTimeout)
{
	static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
	if (iZoneId < 0)
	{
		return FailNamed<Res, Req>(OI_ERR_FAIL);
	}
	return CallNamed<Res>(this, sServiceName, ServicePick::Zone, std::to_string(iZoneId), std::move(stRequest), stTimeout);
}

// 命名调用与直连共用同一个解包等待器，响应不经过 RpcResult 中转或复制。
template <typename Req>
OI::Coro::TaskResult<int32_t> RunNamedOutput(::OI::Net::SessionManager* pManager, std::string sName,
    ServicePick iPick, std::string sZone, std::optional<Req> stRequest,
    typename RpcProtoId<Req>::Response& stResponse, RpcDeadline stDeadline)
{
	RpcRequestRelease<Req> stRelease{stRequest};
	RouteAwaiter stWait;
	stWait.stDeadline = stDeadline;
	stWait.pManager = pManager;
	stWait.sName = std::move(sName);
	stWait.iPick = iPick;
	stWait.sZone = std::move(sZone);
	ServiceRoute stRoute = co_await stWait;
	if (stRoute.iStatus != ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
	{
		co_return RouteError(stRoute.iStatus);
	}
	if (stDeadline.Expired())
	{
		co_return OI_ERR_TIMEOUT;
	}
	auto stCall = MakeRpcCallAwaiter(*stRoute.vClients[0], *stRequest, stResponse, stDeadline.Remaining());
	co_return co_await stCall;
}
template <typename Req>
OI::Coro::TaskResult<int32_t> CallNamedOutput(::OI::Net::SessionManager* pManager, std::string sName,
    ServicePick iPick, std::string sZone, Req stRequest,
    typename RpcProtoId<Req>::Response& stResponse, RpcDeadline stDeadline)
{
	return RunNamedOutput<Req>(pManager, std::move(sName), iPick, std::move(sZone),
	    std::optional<Req>(std::in_place, std::move(stRequest)), stResponse, stDeadline);
}

inline OI::Coro::TaskResult<int32_t> FailRpcOutput(int32_t iCode)
{
	co_return iCode;
}

template <typename Req>
[[nodiscard]] auto ProtoClientManager::Call(Req stRequest, typename RpcProtoId<Req>::Response& stResponse,
    ::OI::Timer::MS stTimeout)
{
	// 不指定分区时沿用当前客户端的连接、首次建连排队和固定配置。
	return RpcCall(*this, std::move(stRequest), stResponse, stTimeout);
}

template <typename Req>
[[nodiscard]] auto ProtoClientManager::CallAny(Req stRequest, typename RpcProtoId<Req>::Response& stResponse,
    ::OI::Timer::MS stTimeout)
{
	return CallNamedOutput(this, sServiceName, ServicePick::Any, std::string(), std::move(stRequest), stResponse, stTimeout);
}

template <typename Req>
[[nodiscard]] auto ProtoClientManager::Call(int32_t iZoneId, Req stRequest,
    typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout)
{
	if (iZoneId < 0)
	{
		return FailRpcOutput(OI_ERR_FAIL);
	}
	return CallNamedOutput(this, sServiceName, ServicePick::Zone, std::to_string(iZoneId), std::move(stRequest), stResponse, stTimeout);
}

template <typename Req>
[[nodiscard]] OI::Coro::TaskResult<int32_t> RpcCall(const std::string& sService, Req stRequest,
    typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	// 本函数不是协程，解析/拷贝在返回前完成；任务帧按值保存选路参数。
	ServiceRoute stRoute;
	std::string sName;
	std::string sZone;
	ServicePick iPick = ServicePick::Any;
	if (PrepareTarget(sService, false, stRoute, sName, iPick, sZone))
	{
		return FailRpcOutput(OI_ERR_FAIL);
	}
	return CallNamedOutput(nullptr, std::move(sName), iPick, std::move(sZone), std::move(stRequest), stResponse, stTimeout);
}

template <typename Res, typename Req>
OI::Coro::TaskResult<std::vector<RpcResult<Res>>> SendRouted(ServiceRoute stRoute, Req stRequest, RpcDeadline stDeadline, size_t iFailureCount = 1)
{
	static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
	if (stRoute.iStatus != ServiceRouteStatus::Ok)
	{
		int32_t iCode = RouteError(stRoute.iStatus);
		co_return std::vector<RpcResult<Res>>(iFailureCount, RpcResult<Res>(iCode, Res{}));
	}
	std::deque<std::optional<RpcValueAwaiter<Req, Res>>> vCalls;
	std::vector<int32_t> vSkipped(stRoute.vClients.size(), OI_ERR_CONNECT);
	size_t iSlot = 0;
	std::exception_ptr stBuildError;
	try
	{
		for (auto* pClient : stRoute.vClients)
		{
			const size_t iCurrentSlot = iSlot++;
			if (!pClient)
			{
				vCalls.emplace_back();
				continue;
			}
			if (stDeadline.Expired())
			{
				vSkipped[iCurrentSlot] = OI_ERR_TIMEOUT;
				vCalls.emplace_back();
				continue;
			}
			vCalls.emplace_back(std::in_place, RpcCall(*pClient, stRequest, stDeadline.Remaining()));
			vCalls.back()->Submit();
		}
	}
	catch (...)
	{
		stBuildError = std::current_exception();
	}
	if (stBuildError)
	{
		for (auto& stCall : vCalls)
		{
			if (!stCall || !stCall->bDispatched)
			{
				continue;
			}
			try
			{
				(void)co_await *stCall;
			}
			catch (...)
			{
			}
		}
		std::rethrow_exception(stBuildError);
	}
	std::vector<RpcResult<Res>> vResults;
	vResults.reserve(vCalls.size());
	for (auto& stCall : vCalls)
	{
		if (!stCall)
		{
			vResults.push_back(RpcResult<Res>(vSkipped[vResults.size()], Res{}));
			continue;
		}
		try
		{
			vResults.push_back(co_await *stCall);
		}
		catch (const std::exception&)
		{
			vResults.push_back(RpcResult<Res>(OI_ERR_FAIL, Res{}));
		}
	}
	co_return vResults;
}

// 名字拷进任务。名单是任务跑起来时的注册表回包，等待不占线程。
// 路由在执行期间持有连接租约，完成后释放；回包仍按调用号匹配等待器。
template <typename Res, typename Req>
OI::Coro::TaskResult<std::vector<RpcResult<Res>>> CallRouted(RouteAwaiter::Kind iKind, ::OI::Net::SessionManager* pManager, std::string sName, ServicePick iPick, std::string sZone, std::vector<int32_t> vZones, std::string sTarget, bool bBatch, Req stRequest, RpcDeadline stDeadline)
{
	// 指定分区时，整体错误也必须保留每个输入槽（包括重复分区）。
	const size_t iFailureCount = iKind == RouteAwaiter::Kind::Zones ? vZones.size() : 1;
	RouteAwaiter stWait;
	stWait.stDeadline = stDeadline;
	stWait.iKind = iKind;
	stWait.pManager = pManager;
	stWait.sName = std::move(sName);
	stWait.iPick = iPick;
	stWait.sZone = std::move(sZone);
	stWait.vZones = std::move(vZones);
	stWait.sTarget = std::move(sTarget);
	stWait.bBatch = bBatch;
	ServiceRoute stRoute = co_await stWait;
	auto stBatch = SendRouted<Res>(std::move(stRoute), std::move(stRequest), stDeadline, iFailureCount);
	co_return co_await stBatch;
}

template <typename Res, typename Req>
OI::Coro::TaskResult<std::vector<RpcResult<Res>>> RpcCallBatch(const std::string& sService, Req stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return CallRouted<Res>(RouteAwaiter::Kind::Target, nullptr, std::string(), ServicePick::Any, std::string(), {}, sService, true, std::move(stRequest), stTimeout);
}

template <typename Res, typename Req>
OI::Coro::TaskResult<RpcResult<Res>> RunTargetCall(std::string sService, Req stRequest, RpcDeadline stDeadline)
{
	static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
	RouteAwaiter stWait;
	stWait.stDeadline = stDeadline;
	stWait.iKind = RouteAwaiter::Kind::Target;
	stWait.sTarget = std::move(sService);
	stWait.bBatch = false;
	ServiceRoute stRoute = co_await stWait;
	if (stRoute.iStatus != ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
	{
		int32_t iCode = RouteError(stRoute.iStatus);
		co_return RpcResult<Res>(iCode, Res{});
	}
	if (stDeadline.Expired())
	{
		co_return RpcResult<Res>(OI_ERR_TIMEOUT, Res{});
	}
	auto stCall = RpcCall(*stRoute.vClients[0], std::move(stRequest), stDeadline.Remaining());
	co_return co_await stCall;
}

template <typename Res, typename Req>
OI::Coro::TaskResult<RpcResult<Res>> RpcCall(std::string sService, Req stRequest, ::OI::Timer::MS stTimeout = ::OI::Timer::Seconds(180))
{
	return RunTargetCall<Res>(std::move(sService), std::move(stRequest), RpcDeadline(stTimeout));
}

template <typename Res, typename Req>
auto ProtoClientManager::CallAll(const Req& stRequest, ::OI::Timer::MS stTimeout)
{
	return CallRouted<Res>(RouteAwaiter::Kind::Named, this, sServiceName, ServicePick::All, std::string(), {}, std::string(), false, stRequest, stTimeout);
}

template <typename Res, typename Req>
auto ProtoClientManager::CallMany(const std::vector<int32_t>& vZoneIds, const Req& stRequest, ::OI::Timer::MS stTimeout)
{
	return CallRouted<Res>(RouteAwaiter::Kind::Zones, this, sServiceName, ServicePick::Any, std::string(), vZoneIds, std::string(), false, stRequest, stTimeout);
}

}
}

// 业务命名约定：协议 Name 对应生成的 NameReq / NameRes。
// 可用于函数声明或定义；stRequest/stResponse/stContext 是宏提供的参数名。
#define OI_RPC_HANDLER(Name, Function) \
	::OI::Rpc::RpcTask Function(const ::OI::RpcData::Name##Req& stRequest, ::OI::RpcData::Name##Res& stResponse)
#define OI_RPC_SYNC_HANDLER(Name, Function) \
	int32_t Function(const ::OI::RpcData::Name##Req& stRequest, ::OI::RpcData::Name##Res& stResponse)
#define OI_RPC_CONTEXT_HANDLER(Name, Function) \
	::OI::Rpc::RpcTask Function(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::Name##Req& stRequest, ::OI::RpcData::Name##Res& stResponse)
#define OI_RPC_BIND(Name, ...) \
	::OI::Rpc::HandleRpc<::OI::RpcData::Name##Req>((__VA_ARGS__))
// 从本次回调的上下文拿回组件。stManager 必须就是这个类型，对象要活过所有在途请求。
#define OI_RPC_SELF(Type) static_cast<Type&>(stContext.stManager)
// 注册成员函数。不在这里绑 this，处理时用上下文还原对象再调用。成员签名与 OI_RPC_CONTEXT_HANDLER 相同。
#define OI_RPC_BIND_SELF(Name, Type, Member) \
	::OI::Rpc::HandleRpc<::OI::RpcData::Name##Req>([](const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::Name##Req& stRequest, ::OI::RpcData::Name##Res& stResponse) -> decltype(auto) \
	{ \
		return OI_RPC_SELF(Type).Member(stContext, stRequest, stResponse); \
	})

// 单向消息。协议 Name 只用 NameReq，生成的 NameRes 不会出现在线上。
#define OI_NOTIFY_HANDLER(Name, Function) \
	::OI::Rpc::RpcTask Function(const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::Name##Req& stRequest)
#define OI_NOTIFY_BIND(Name, ...) \
	::OI::Rpc::HandleNotify<::OI::RpcData::Name##Req>((__VA_ARGS__))
// 注册成员函数，签名与 OI_NOTIFY_HANDLER 相同。
#define OI_NOTIFY_BIND_SELF(Name, Type, Member) \
	::OI::Rpc::HandleNotify<::OI::RpcData::Name##Req>([](const ::OI::Rpc::RpcContext& stContext, const ::OI::RpcData::Name##Req& stRequest) -> decltype(auto) \
	{ \
		return OI_RPC_SELF(Type).Member(stContext, stRequest); \
	})
