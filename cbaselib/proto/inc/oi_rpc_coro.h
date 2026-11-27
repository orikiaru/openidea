#pragma once
#include "oi_bytes_stream.h"
#include "oi_header.h"
#include "oi_parser.h"
#include "oi_rpc_call.h"
#include "oi_registry.h"
#include "oi_session_manager.h"
#include "oi_lock.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_duration.h"
#include <algorithm>
#include <atomic>
#include <coroutine>
#include <cstdio>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "oi_coro.h"

namespace OI
{
namespace Rpc
{
// 标准服务端协程：co_return 0 成功，正数为业务错误。
using RpcTask = OI::Coro::TaskResult<int32_t>;

// 定义在下面。会话不在时由 SendBytesResult 返回 OI_ERR_CONNECT，编码失败是 OI_ERR_ENCODE。
template <typename Msg>
int32_t RpcNotify(::OI::Net::SessionManager& stManager, sid_t iSessionId, const Msg& stMessage);

// 仅需要连接信息的处理函数接收上下文；生命周期覆盖本次处理协程。
struct RpcContext
{
	// 处理这条请求的任务标识。
	OI::Coro::Token iToken;
	// 请求从哪个管理器进来。
	::OI::Net::SessionManager& stManager;
	// 请求所在的会话。回包发回这里。
	sid_t iSessionId;

	// 发回这次请求所在的会话。
	template <typename Msg>
	int32_t Notify(const Msg& stMessage) const
	{
		return RpcNotify(stManager, iSessionId, stMessage);
	}
	// 仍用这次请求的管理器，发给另一条会话。
	template <typename Msg>
	int32_t Notify(sid_t iSessionIdLocal, const Msg& stMessage) const
	{
		return RpcNotify(stManager, iSessionIdLocal, stMessage);
	}
};

template <typename T>
struct RpcProtoId;

// 请求和回包共用一个协议号。调用号最高位是 1 表示请求，回包清掉这一位。
// 请求：调用号 + Req；回执：调用号 + int32_t retcode + 成功时的 Res。
// 0 成功，负数保留给框架错误，正数用于业务错误。失败不序列化 Res。

// 服务端处理函数的包装：解请求、起任务、发回包。
class RpcHandlerBuilder
{
  public:
	// 业务任务外面这一层：co_await 业务任务，再把带调用号的回包写出去。
	template <typename Req, typename Res, typename F>
	static OI::Coro::Task Run(F fnFunction, proto_type_t iResponseId, OI::Coro::Token iToken, ::OI::Net::SessionManager* pManager, sid_t iSessionId, uint64_t iRawCallId, Req stRequestLocal)
	{
		// 单向消息发到了请求处理函数上：照常处理，不回包，免得回执被对端当成又一条单向消息。
		const bool bNotify = iRawCallId == 0;
		const uint64_t iCallIdLocal = iRawCallId & ~(uint64_t)RPC_REQ_MASK;
		Res stResponseLocal{};
		int32_t iRetcode = 0;
		const char* pStage = "handler";
		try
		{
			RpcContext stContext{iToken, *pManager, iSessionId};
			auto stInvoke = [pFunction = &fnFunction, pRequest = &stRequestLocal, pResponse = &stResponseLocal, pContext = &stContext]()
			{
				(void)pContext;
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
			RpcCallManager::ReportError(iToken, iCallIdLocal, iSessionId, iResponseId, pStage, std::current_exception());
		}
		catch (...)
		{
			iRetcode = OI_ERR_FAIL;
			RpcCallManager::ReportError(iToken, iCallIdLocal, iSessionId, iResponseId, pStage, std::current_exception());
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
			RpcCallManager::ReportError(iToken, iCallIdLocal, iSessionId, iResponseId, "encode_response", std::current_exception());
			OI::Coro::ReportTaskError(iToken, std::current_exception());
			// 丢弃已经写入的半包，改回只有调用号和错误码的回执。
			RpcCallManager::SendFailureReply(iToken, pManager, iSessionId, iCallIdLocal, iResponseId);
			co_return;
		}
		RpcCallManager::SendReply(iToken, pManager, iSessionId, iCallIdLocal, iResponseId, stFrame.GetMutableBuff());
	}

	// 请求解不开时直接回失败，不进处理函数。
	template <typename Req, typename Res, typename F>
	static RpcHandleFn Make(F fnFunction, proto_type_t iResponseId)
	{
		return [fnFunction = std::move(fnFunction), iResponseId](::OI::Base::BytesStream& stBody, RpcDelivery&& stDelivery) mutable
		{
			std::optional<Req> stRequest;
			try
			{
				stRequest.emplace();
				stRequest->deserialize(stBody);
			}
			catch (...)
			{
				const uint64_t iCallId = stDelivery.iRawCallId & ~(uint64_t)RPC_REQ_MASK;
				RpcCallManager::ReportError(0, iCallId, stDelivery.iSessionId, iResponseId, "decode_request", std::current_exception());
				if (stDelivery.iRawCallId != 0)
				{
					RpcCallManager::SendFailureReply(0, stDelivery.pManager, stDelivery.iSessionId, iCallId, iResponseId);
				}
				return;
			}
			OI::Coro::StartTask([fnFunction, iResponseId, stDelivery = std::move(stDelivery), stRequest = std::move(*stRequest)](OI::Coro::Token iToken) mutable
			{
				return Run<Req, Res>(fnFunction, iResponseId, iToken, stDelivery.pManager,
				    stDelivery.iSessionId, stDelivery.iRawCallId, std::move(stRequest));
			});
		};
	}
};

// 从处理函数的形参取出请求和回包类型。只认非模板的 lambda、函数、函数指针和成员函数；带 auto 形参的写 RegisterRpcHandler<Req>。
template <typename T>
struct RpcCallableArgs
{
};
template <typename C, typename R, typename... A>
struct RpcCallableArgs<R (C::*)(A...) const>
{
	using Args = std::tuple<A...>;
	using Class = C;
	using Return = R;
};
template <typename C, typename R, typename... A>
struct RpcCallableArgs<R (C::*)(A...)>
{
	using Args = std::tuple<A...>;
	using Class = C;
	using Return = R;
};
template <typename R, typename... A>
struct RpcCallableArgs<R (*)(A...)>
{
	using Args = std::tuple<A...>;
};
template <typename F, typename = void>
struct RpcHandlerArgs : RpcCallableArgs<F>
{
};
template <typename F>
struct RpcHandlerArgs<F, std::void_t<decltype(&F::operator())>> : RpcCallableArgs<decltype(&F::operator())>
{
};

template <typename Args>
struct RpcHandlerTypes
{
};
template <typename Req, typename Res>
struct RpcHandlerTypes<std::tuple<Req, Res>>
{
	using Request = std::remove_cvref_t<Req>;
	using Response = std::remove_cvref_t<Res>;
};
template <typename Ctx, typename Req, typename Res>
    requires std::is_same_v<std::remove_cvref_t<Ctx>, RpcContext>
struct RpcHandlerTypes<std::tuple<Ctx, Req, Res>>
{
	using Request = std::remove_cvref_t<Req>;
	using Response = std::remove_cvref_t<Res>;
};

template <typename Args>
struct RpcNotifyTypes
{
};
template <typename Msg>
struct RpcNotifyTypes<std::tuple<Msg>>
{
	using Message = std::remove_cvref_t<Msg>;
};
template <typename Ctx, typename Msg>
    requires std::is_same_v<std::remove_cvref_t<Ctx>, RpcContext>
struct RpcNotifyTypes<std::tuple<Ctx, Msg>>
{
	using Message = std::remove_cvref_t<Msg>;
};

template <typename F>
constexpr bool kRpcHandlerDeducible = requires { typename RpcHandlerTypes<typename RpcHandlerArgs<F>::Args>::Request; };
template <typename F>
constexpr bool kRpcNotifyDeducible = requires { typename RpcNotifyTypes<typename RpcHandlerArgs<F>::Args>::Message; };

// 发一条单向消息，不等回包。返回 SendBytesResult 的错误码，编码失败是 OI_ERR_ENCODE。
template <typename Msg>
int32_t RpcNotify(::OI::Net::SessionManager& stManager, sid_t iSessionId, const Msg& stMessage)
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RpcNotify");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

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
	return stManager.SendFrame(iSessionId, stFrame.GetMutableBuff());
}

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
	bool bDispatched = false;               // Dispatch 已开始。成功登记后 iCallId 才有效。
	Res stDecoded{};
	int32_t iReplyCode = OI_ERR_FAIL;
	std::exception_ptr stDecodeError;
	bool bReplied;
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
		RpcCallManager::GetInstance().Abandon(iCallId);
		RpcCallManager::WaitGuard(pGuard);
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
	void DecodeReply(::OI::Base::BytesStream& stInput, sid_t iSourceSession)
	{
		iReplySessionId = iSourceSession;
		try
		{
			stInput >> iReplyCode;
			if (iReplyCode == 0)
			{
				stDecoded.deserialize(stInput);
			}
		}
		catch (...)
		{
			iReplyCode = OI_ERR_FAIL;
			stDecodeError = std::current_exception();
		}
		bReplied = true;
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
		iTimeoutMilliseconds = NormalizeRpcTimeoutMilliseconds(iTimeoutMilliseconds);
		iResult = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RpcCall::Dispatch");
		if (iResult != OI_OK)
		{
			iGate.store(OI::Coro::Gate::Ready, std::memory_order_release);
			return;
		}
		iResult = OI_ERR_FAIL;
		bReplied = false;
		stSubmissionError = nullptr;
		iGate.store(OI::Coro::Gate::Idle, std::memory_order_relaxed);
		iCallId = 0;
		pGuard = std::make_shared<std::atomic<int32_t>>(0);
		uint64_t iCallIdLocal = RpcCallManager::GetInstance().Bind(pManager, iSessionId, RpcProtoId<Res>::iValue, stHandle, &iResult, iTimeoutMilliseconds, [this](::OI::Base::BytesStream& stInput, sid_t iSourceSession)
		{
			DecodeReply(stInput, iSourceSession);
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
				const int32_t iRet = pManager->SendFrame(iSessionId, stFrame.GetMutableBuff());
				if (iRet != OI_OK)
				{
					RpcCallManager::GetInstance().Wake(iCallIdLocal, iRet, NULL);
				}
			}
		}
		catch (...)
		{
			// 不直接从 await_suspend 抛出：超时/断连可能已取走等待项，正在使用 this。
			// 先完成等待，再由同一 gate 确认回调已结束，最后在 await_resume 重抛。
			stSubmissionError = std::current_exception();
			RpcCallManager::GetInstance().Wake(iCallIdLocal, OI_ERR_FAIL, nullptr);
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
			iResult = iReplyCode;
			if (stDecodeError)
			{
				RpcCallManager::ReportError(iAwaitToken, iCallId, iReplySessionId, RpcProtoId<Res>::iValue, "decode_response", stDecodeError);
			}
			if (iResult == 0)
			{
				*pResponse = std::move(stDecoded);
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
// 同一调用的查询、建连和回包共享单调时钟预算。非正值打错误日志并按 30 秒计。
struct RpcDeadline
{
	int64_t iDeadlineMilliseconds = 0;
	RpcDeadline() = default;
	RpcDeadline(::OI::Timer::MS stTimeout);
	template <typename Duration, std::enable_if_t<std::is_convertible_v<Duration, ::OI::Timer::MS>, int> = 0>
	RpcDeadline(Duration stTimeout)
	    : RpcDeadline(::OI::Timer::MS(stTimeout))
	{
	}
	bool Expired() const;
	// 没有期限返回 0，否则至少 1 毫秒。
	::OI::Timer::MS Remaining() const;
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

struct RoutePoolStats
{
	size_t iGroups = 0;
	size_t iClients = 0;
};

enum class ServicePick
{
	Any,
	Zone,
	All,
};

// 挂起等注册表，不在调用线程里同步等。名字非法时不挂起。
struct RouteAwaiter
{
	enum class Kind
	{
		Named,
		Zones,
	};
	RpcDeadline stDeadline;
	Kind iKind = Kind::Named;
	std::string sName;
	std::string sZone;
	ServicePick iPick = ServicePick::Any;
	std::vector<int32_t> vZones;
	bool bResolved = false;
	int32_t iCode = OI_ERR_CONNECT;                 // 读缓存的结果。exchange(Ready) 之前写完。
	std::vector<::OI::Base::ServiceInstance> vList; // 缓存里的名单。和 iCode 一起在认领完成前写完。
	std::vector<std::string> vZoneNames;
	ServiceRoute stRoute;
	std::coroutine_handle<> stHandle{}; // 调用方句柄。门已经是 Waiting 才 Schedule。
	// 完成门。Idle 还在等拉取，Ready 回包已写入，Waiting 调用方已挂起。
	std::atomic<OI::Coro::Gate> iGate{OI::Coro::Gate::Idle};

	bool await_ready() const noexcept
	{
		return false;
	}
	bool await_suspend(std::coroutine_handle<> stHandleLocal);
	// 按名单向 RouteManager 取发送目标。
	ServiceRoute await_resume();

  private:
	// 返回 true 表示名字非法，不用再问注册表，结果已经写进 stRoute。
	bool Prepare();
	// 读 RegistryClient 的本地缓存，不发请求。命中返回 true；没拉到过就挂定时器复查，到点写 iCode、vList 后过完成门。
	bool ReadCache(int64_t iTimeoutMilliseconds);
	void Recheck(int64_t iDeadlineMilliseconds);
	void Finish();
	static bool ValidToken(const std::string& sText);
};

// 按服务名选路后发送，RouteManager 的调用都落到这里。
class RouteCall
{
  public:
	// 选路失败的状态换成返回码。
	static int32_t Error(ServiceRouteStatus iStatus);
	// 直接以 iCode 完成的任务，用于参数不合法时不发请求。
	static OI::Coro::TaskResult<int32_t> Fail(int32_t iCode);

	// 按名字选出一台再发一次，响应直接写进调用方对象。
	template <typename Req>
	static OI::Coro::TaskResult<int32_t> RunOutput(std::string sName, ServicePick iPick, std::string sZone, std::optional<Req> stRequest,
	    typename RpcProtoId<Req>::Response& stResponse, RpcDeadline stDeadline)
	{
		RpcRequestRelease<Req> stRelease{stRequest};
		RouteAwaiter stWait;
		stWait.stDeadline = stDeadline;
		stWait.sName = std::move(sName);
		stWait.iPick = iPick;
		stWait.sZone = std::move(sZone);
		ServiceRoute stRoute = co_await stWait;
		if (stRoute.iStatus != ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
		{
			co_return Error(stRoute.iStatus);
		}
		if (stDeadline.Expired())
		{
			co_return OI_ERR_TIMEOUT;
		}
		auto stCall = MakeRpcCallAwaiter(*stRoute.vClients[0], *stRequest, stResponse, stDeadline.Remaining());
		co_return co_await stCall;
	}

	// 给 stRoute 里每个目标各发一份，先全部发出再等，结果和目标一一对应。空目标那一格记 OI_ERR_CONNECT。
	// 选路失败时返回 iFailureCount 格同一个错误码。
	template <typename Res, typename Req>
	static OI::Coro::TaskResult<std::vector<RpcResult<Res>>> Send(ServiceRoute stRoute, Req stRequest, RpcDeadline stDeadline, size_t iFailureCount = 1)
	{
		static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
		if (stRoute.iStatus != ServiceRouteStatus::Ok)
		{
			int32_t iCode = Error(stRoute.iStatus);
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
				// Bind 失败时没有等待项，也不会有人唤醒这一格。
				if (!stCall || stCall->iCallId == 0)
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

	// 名字拷进任务。名单是任务跑起来时的注册表缓存，等待不占线程。
	// 路由在执行期间持有发送目标，完成后释放；回包仍按调用号匹配等待器。
	template <typename Res, typename Req>
	static OI::Coro::TaskResult<std::vector<RpcResult<Res>>> Call(RouteAwaiter::Kind iKind, std::string sName, ServicePick iPick, std::vector<int32_t> vZones,
	    Req stRequest, RpcDeadline stDeadline)
	{
		// 指定分区时，整体错误也必须保留每个输入槽（包括重复分区）。
		const size_t iFailureCount = iKind == RouteAwaiter::Kind::Zones ? vZones.size() : 1;
		RouteAwaiter stWait;
		stWait.stDeadline = stDeadline;
		stWait.iKind = iKind;
		stWait.sName = std::move(sName);
		stWait.iPick = iPick;
		stWait.vZones = std::move(vZones);
		ServiceRoute stRoute = co_await stWait;
		auto stBatch = Send<Res>(std::move(stRoute), std::move(stRequest), stDeadline, iFailureCount);
		co_return co_await stBatch;
	}

	// 选路后给每个目标发一份单向消息并等回执。选路失败或有目标失败就抛出，由任务错误处理器报告服务名、失败格数和首个返回码。
	template <typename Msg>
	static OI::Coro::Task RunNotify(RouteAwaiter::Kind iKind, std::string sName, ServicePick iPick, std::string sZone, std::vector<int32_t> vZones, Msg stMessage)
	{
		using Res = typename RpcProtoId<Msg>::Response;
		const RpcDeadline stDeadline(::OI::Timer::Seconds(180));
		const size_t iFailureCount = iKind == RouteAwaiter::Kind::Zones ? vZones.size() : 1;
		RouteAwaiter stWait;
		stWait.stDeadline = stDeadline;
		stWait.iKind = iKind;
		stWait.sName = sName;
		stWait.iPick = iPick;
		stWait.sZone = sZone;
		stWait.vZones = vZones;
		ServiceRoute stRoute = co_await stWait;
		auto stBatch = Send<Res>(std::move(stRoute), std::move(stMessage), stDeadline, iFailureCount);
		std::vector<RpcResult<Res>> vResults = co_await stBatch;
		size_t iFailed = 0;
		int32_t iFirstCode = OI_OK;
		std::string sFailedZones;
		for (size_t iIndexLocal = 0; iIndexLocal < vResults.size(); ++iIndexLocal)
		{
			const int32_t iCode = vResults[iIndexLocal].GetRet();
			if (iCode == OI_OK)
			{
				continue;
			}
			if (iFailed == 0)
			{
				iFirstCode = iCode;
			}
			++iFailed;
			if (iKind == RouteAwaiter::Kind::Zones && iIndexLocal < vZones.size())
			{
				sFailedZones += (sFailedZones.empty() ? "" : ",") + std::to_string(vZones[iIndexLocal]);
			}
		}
		if (iFailed != 0)
		{
			throw std::runtime_error("route notification failed service=" + sName + (sZone.empty() ? "" : " zone=" + sZone) +
			                         (sFailedZones.empty() ? "" : " zones=" + sFailedZones) + " protocol=" + std::to_string(RpcProtoId<Msg>::iValue) +
			                         " failed=" + std::to_string(iFailed) + "/" + std::to_string(vResults.size()) + " code=" + std::to_string(iFirstCode));
		}
	}

	// 启动一条通知任务就返回 OI_OK；线程不对返回错误码。
	template <typename Msg>
	static int32_t StartNotify(RouteAwaiter::Kind iKind, std::string sName, ServicePick iPick, std::string sZone, std::vector<int32_t> vZones, const Msg& stMessage)
	{
		const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RouteCall::StartNotify");
		if (iThreadStatus != OI_OK)
		{
			return iThreadStatus;
		}
		OI::Coro::StartTask([iKind, sName = std::move(sName), iPick, sZone = std::move(sZone), vZones = std::move(vZones), stMessage]() mutable
		{
			return RunNotify(iKind, std::move(sName), iPick, std::move(sZone), std::move(vZones), std::move(stMessage));
		});
		return OI_OK;
	}
};

template <typename Res>
struct PeerResult
{
	int32_t iInstanceId;
	RpcResult<Res> stResult;
};

// 按实例发送，ProtoMultiClientManager 的调用都落到这里。
class PeerCall
{
  public:
	// 调用期间持有发送目标，实例中途退役也不会释放它。
	template <typename Req>
	static OI::Coro::TaskResult<int32_t> RunOutput(std::shared_ptr<RpcSender> pSender, std::optional<Req> stRequest,
	    typename RpcProtoId<Req>::Response& stResponse, ::OI::Timer::MS stTimeout)
	{
		RpcRequestRelease<Req> stRelease{stRequest};
		if (!pSender)
		{
			co_return OI_ERR_CONNECT;
		}
		auto stCall = MakeRpcCallAwaiter(*pSender, *stRequest, stResponse, stTimeout);
		co_return co_await stCall;
	}

	template <typename Res, typename Req>
	static OI::Coro::TaskResult<std::vector<PeerResult<Res>>> RunBatch(std::vector<int32_t> vIds, ServiceRoute stRoute, Req stRequest, RpcDeadline stDeadline)
	{
		auto stBatch = RouteCall::Send<Res>(std::move(stRoute), std::move(stRequest), stDeadline, 0);
		std::vector<RpcResult<Res>> vResults = co_await stBatch;
		std::vector<PeerResult<Res>> vPeers;
		vPeers.reserve(vResults.size());
		for (size_t iIndexLocal = 0; iIndexLocal < vResults.size() && iIndexLocal < vIds.size(); ++iIndexLocal)
		{
			vPeers.push_back(PeerResult<Res>{vIds[iIndexLocal], std::move(vResults[iIndexLocal])});
		}
		co_return vPeers;
	}

	template <typename Msg>
	static OI::Coro::Task RunNotify(std::shared_ptr<RpcSender> pSender, Msg stMessage)
	{
		auto stCall = RpcCall(*pSender, std::move(stMessage), ::OI::Timer::Seconds(180));
		auto stResult = co_await stCall;
		if (stResult.GetRet() != OI_OK)
		{
			throw std::runtime_error("RPC notification failed protocol=" + std::to_string(RpcProtoId<Msg>::iValue) + " code=" + std::to_string(stResult.GetRet()) + " call=" + std::to_string(stCall.iCallId));
		}
	}

	template <typename Msg>
	static int32_t StartNotify(std::shared_ptr<RpcSender> pSender, const Msg& stMessage)
	{
		const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("PeerCall::StartNotify");
		if (iThreadStatus != OI_OK)
		{
			return iThreadStatus;
		}

		if (!pSender)
		{
			return OI_ERR_CONNECT;
		}
		OI::Coro::StartTask([pSender = std::move(pSender), stMessage]() mutable
		{
			return RunNotify(std::move(pSender), std::move(stMessage));
		});
		return OI_OK;
	}

	// 给 stRoute.vOwners 里每个非空目标各启动一条通知任务，返回启动了几条；线程不对返回错误码。
	template <typename Msg>
	static int32_t StartBatchNotify(const ServiceRoute& stRoute, const Msg& stMessage)
	{
		const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("PeerCall::StartBatchNotify");
		if (iThreadStatus != OI_OK)
		{
			return iThreadStatus;
		}
		int32_t iStarted = 0;
		for (const auto& pSender : stRoute.vOwners)
		{
			if (!pSender)
			{
				continue;
			}
			OI::Coro::StartTask([pSender, stMessage]() mutable
			{
				return RunNotify(std::move(pSender), std::move(stMessage));
			});
			++iStarted;
		}
		return iStarted;
	}
};

}
}
