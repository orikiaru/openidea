#pragma once
#include "oi_bytes_stream.h"
#include "oi_header.h"
#include "oi_parser.h"
#include "oi_rpc_call.h"
#include "oi_session_manager.h"
#include "oi_rpc_session_manager.h"
#include "oi_lock.h"
#include "oi_duration.h"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace coro
{
// 协程 token。StartTask 生成并传进协程。
typedef uint64_t Token;
typedef Token TaskUid;

TaskUid AllocTaskUid();

// 挂起的协程放回运行队列。单线程进本线程队列，多线程进工人队列。RPC 回包和定时器都走这里。
void Schedule(std::coroutine_handle<> stHandleParam);

// 根协程跑完后不能在 final_suspend 里销毁自己。登记下来，等 resume 返回再释放。
void DetachFinished(std::coroutine_handle<> stHandleParam);
void DrainDetached();
using TaskErrorHandler = void (*)(Token, std::exception_ptr);
// 在工作线程调用；处理函数必须线程安全，不允许抛异常。nullptr 恢复默认 stderr 报告。
void SetTaskErrorHandler(TaskErrorHandler fnHandlerParam);
void ReportTaskError(Token iTokenParam, std::exception_ptr stErrorParam) noexcept;

// 单线程就绪队列。Poll 只跑进入时已经在队列里的句柄，这一轮新挂上的留到 poll 之后。
class StScheduler
{
public:
	static void Post(std::coroutine_handle<> stHandleParam);
	static bool Poll();
	static bool Queued();
};

// 下一轮再跑，或者过 ms 毫秒再跑。到点就是 Schedule。
struct SleepMs
{
	int32_t iMilliseconds;
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> stHandleParam) const noexcept;
	void await_resume() const noexcept {}
};

inline SleepMs yield(int32_t iMillisecondsParam)
{
	return SleepMs{iMillisecondsParam};
}

inline SleepMs yield(std::chrono::milliseconds stDelayParam)
{
	if (stDelayParam.count() > (std::numeric_limits<int32_t>::max)()) throw std::out_of_range("yield duration too large");
	return yield(stDelayParam.count() <= 0 ? 0 : static_cast<int32_t>(stDelayParam.count()));
}

// 子任务跑完时：调用方已经挂起，就把调用方 Schedule 回去；还没挂起，就只做个标记。
struct TaskFinalSuspend
{
	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	void await_suspend(std::coroutine_handle<Promise> stHandleParam) const noexcept
	{
		auto& stPromise = stHandleParam.promise();
		if (stPromise.bReady.exchange(true, std::memory_order_acq_rel))
		{
			if (stPromise.stContinuation)
			{
				Schedule(stPromise.stContinuation);
			}
			return;
		}
		if (stPromise.bDetached)
		{
			DetachFinished(stHandleParam);
		}
	}
	void await_resume() const noexcept {}
};

struct TaskState
{
	// 保活 StartTask 的闭包；协程 lambda 的捕获仍属于闭包对象。
	std::shared_ptr<void> pCallable;
	std::exception_ptr stError;
	Token iTaskId;
	std::coroutine_handle<> stContinuation;
	bool bDetached;
	std::atomic<bool> bReady;
	TaskState() : iTaskId(0), stContinuation(), bDetached(false), bReady(false) {}
	std::suspend_always initial_suspend() noexcept { return {}; }
	TaskFinalSuspend final_suspend() noexcept { return {}; }
	void unhandled_exception() noexcept
	{
		stError = std::current_exception();
		if (bDetached) ReportTaskError(iTaskId, stError);
	}
};

// 有返回值和无返回值任务共用同一套调度与完成门，不增加包装协程。
template<typename T>
struct TaskReturn
{
	std::optional<T> stResult;
	void return_value(T stValueParam) { stResult.emplace(std::move(stValueParam)); }
};
template<>
struct TaskReturn<void>
{
	void return_void() noexcept {}
};

// 一条任务。Start 丢进队列；co_await 就是调用它，跑完回到调用方。
template<typename T = void>
struct [[nodiscard]] BasicTask
{
	struct promise_type : TaskState, TaskReturn<T>
	{
		BasicTask get_return_object()
		{
			return BasicTask{std::coroutine_handle<promise_type>::from_promise(*this)};
		}
	};
	std::coroutine_handle<promise_type> stHandle;
	explicit BasicTask(std::coroutine_handle<promise_type> stHandleParam) : stHandle(stHandleParam) {}
	BasicTask(BasicTask&& stOtherParam) noexcept : stHandle(stOtherParam.stHandle) { stOtherParam.stHandle = {}; }
	BasicTask(const BasicTask&) = delete;
	~BasicTask() { if (stHandle) stHandle.destroy(); }
	Token Start()
	{
		if (stHandle.promise().iTaskId == 0)
		{
			stHandle.promise().iTaskId = AllocTaskUid();
		}
		Token iTokenLocal = stHandle.promise().iTaskId;
		// 句柄交给调度器。调用方手里的临时 Task 析构时不能把正在跑的帧释放掉。
		stHandle.promise().bDetached = true;
		std::coroutine_handle<> stHandleLocal = stHandle;
		stHandle = nullptr;
		Schedule(stHandleLocal);
		return iTokenLocal;
	}
	bool await_ready() const noexcept { return !stHandle || stHandle.done(); }
	// 子协程当场跑完就返回 false，调用方不挂起。
	// 子协程自己停住（等回包或定时）时，把调用方记在 continuation 上，结束再 Schedule。
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stParentParam) noexcept
	{
		if (!stHandle)
		{
			return false;
		}
		stHandle.promise().stContinuation = stParentParam;
		stHandle.resume();
		return !stHandle.promise().bReady.exchange(true, std::memory_order_acq_rel);
	}
	T await_resume()
	{
		if (stHandle && stHandle.promise().stError) std::rethrow_exception(stHandle.promise().stError);
		if constexpr (!std::is_void_v<T>)
		{
			if (!stHandle || !stHandle.promise().stResult) throw std::logic_error("Task has no result");
			return std::move(*stHandle.promise().stResult);
		}
	}
};

using Task = BasicTask<void>;
template<typename T>
using TaskResult = BasicTask<T>;

template<typename F>
Token StartTask(F&& fnFunctionParam)
{
	Token iTokenLocal = AllocTaskUid();
	auto stCallable = std::make_shared<std::decay_t<F>>(std::forward<F>(fnFunctionParam));
	auto stTask = (*stCallable)(iTokenLocal);
	stTask.stHandle.promise().pCallable = std::move(stCallable);
	stTask.stHandle.promise().iTaskId = iTokenLocal;
	stTask.Start();
	return iTokenLocal;
}

// 协作停止：不会销毁仍被 IO/定时器引用的帧。模块销毁前必须 await Join。
class StopToken
{
	std::shared_ptr<std::atomic<bool>> m_pStop;
public:
	explicit StopToken(std::shared_ptr<std::atomic<bool>> pStopParam) : m_pStop(std::move(pStopParam)) {}
	bool StopRequested() const { return m_pStop->load(std::memory_order_acquire); }
};

class TaskScope
{
	struct State
	{
		::OI::Thread::Mutex stMutex;
		std::shared_ptr<std::atomic<bool>> pStop = std::make_shared<std::atomic<bool>>(false);
		size_t iActive = 0;
		std::vector<std::coroutine_handle<>> vWaiters;
		bool bClosed = false;
		std::exception_ptr stError;
	};
	std::shared_ptr<State> m_pState = std::make_shared<State>();
	template<typename F>
	static Task Run(std::shared_ptr<State> pStateParam, F fnFunctionParam, Token iTokenParam)
	{
		try { co_await fnFunctionParam(iTokenParam, StopToken(pStateParam->pStop)); }
		catch (...)
		{
			::OI::Thread::MutexGuard stLock(pStateParam->stMutex);
			if (!pStateParam->stError) pStateParam->stError = std::current_exception();
			pStateParam->pStop->store(true, std::memory_order_release);
		}
		std::vector<std::coroutine_handle<>> vWaitersLocal;
		{
			::OI::Thread::MutexGuard stLock(pStateParam->stMutex);
			if (--pStateParam->iActive == 0) vWaitersLocal.swap(pStateParam->vWaiters);
		}
		for (const auto& stWaiter : vWaitersLocal) Schedule(stWaiter);
	}
	struct JoinAwaiter
	{
		std::shared_ptr<State> pState;
		bool await_ready() const noexcept { return false; }
		bool await_suspend(std::coroutine_handle<> stWaiterParam)
		{
			::OI::Thread::MutexGuard stLock(pState->stMutex);
			pState->bClosed = true;
			if (pState->iActive == 0) return false;
			pState->vWaiters.push_back(stWaiterParam);
			return true;
		}
		void await_resume()
		{
			::OI::Thread::MutexGuard stLock(pState->stMutex);
			if (pState->stError) std::rethrow_exception(pState->stError);
		}
	};
	static Task Wait(std::shared_ptr<State> pStateParam)
	{
		co_await JoinAwaiter{std::move(pStateParam)};
	}
public:
	TaskScope() = default;
	TaskScope(const TaskScope&) = delete;
	TaskScope& operator=(const TaskScope&) = delete;
	~TaskScope() { RequestStop(); }
	void RequestStop()
	{
		::OI::Thread::MutexGuard stLock(m_pState->stMutex);
		m_pState->bClosed = true;
		m_pState->pStop->store(true, std::memory_order_release);
	}
	template<typename F>
	Token Start(F&& fnFunctionParam)
	{
		::OI::Thread::MutexGuard stLock(m_pState->stMutex);
		if (m_pState->bClosed) throw std::logic_error("TaskScope is closed");
		Token iTokenLocal = AllocTaskUid();
		auto stTask = Run(m_pState, std::forward<F>(fnFunctionParam), iTokenLocal);
		stTask.stHandle.promise().iTaskId = iTokenLocal;
		++m_pState->iActive;
		return stTask.Start();
	}
	Task Join() { return Wait(m_pState); }
};

}

namespace OI { namespace Rpc {
// 标准服务端协程：co_return 0 成功，正数为业务错误。
using RpcTask = coro::TaskResult<int32_t>;

// 仅需要连接信息的处理函数接收上下文；生命周期覆盖本次处理协程。
struct RpcContext
{
	coro::Token iToken;
	::OI::Net::SessionManager& stManager;
	sid_t iSessionId;
};

template<typename T>
struct RpcProtoId;

void BindRpcHandler(proto_type_t iIdParam, std::function<coro::Task(coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> fnHandleParam);

// 请求和回包共用一个协议号。调用号最高位是 1 表示请求，回包清掉这一位。
// 请求：调用号 + Req；回执：调用号 + int32_t retcode + 成功时的 Res。
// 0 成功，负数保留给框架错误，正数用于业务错误。失败不序列化 Res。

// 业务任务外面这一层：解包，co_await 业务任务，再把带调用号的回包写出去。
template<typename Req, typename Res, typename F>
coro::Task RunHandledRpc(F fnFunctionParam, proto_type_t iResponseIdParam, coro::Token iTokenParam, ::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, ::OI::Base::BytesStream stBodyParam)
{
	uint64_t iCallIdLocal = 0;
	try { stBodyParam >> iCallIdLocal; }
	catch (::OI::Base::ParserException&) { co_return; }
	iCallIdLocal &= ~(uint64_t)RPC_REQ_MASK;
	Req stRequestLocal{};
	Res stResponseLocal{};
	int32_t iRetcode = 0;
	try
	{
		stRequestLocal.deserialize(stBodyParam);
		RpcContext stContext{iTokenParam, *pManagerParam, iSessionIdParam};
		auto stInvoke = [&]() {
			if constexpr (std::is_invocable_v<F&, Req&, Res&>)
				return fnFunctionParam(stRequestLocal, stResponseLocal);
			else if constexpr (std::is_invocable_v<F&, const RpcContext&, Req&, Res&>)
				return fnFunctionParam(stContext, stRequestLocal, stResponseLocal);
			else
				return fnFunctionParam(iTokenParam, pManagerParam, iSessionIdParam, stRequestLocal, stResponseLocal); // 兼容已有五参数回调。
		};
		using Return = decltype(stInvoke());
		if constexpr (std::is_same_v<Return, int32_t>)
		{
			iRetcode = stInvoke();
		}
		else
		{
			static_assert(std::is_same_v<Return, RpcTask> || std::is_same_v<Return, coro::Task>,
				"RPC handler must return RpcTask, int32_t or legacy coro::Task");
			auto stChild = stInvoke();
			if (stChild.stHandle) stChild.stHandle.promise().iTaskId = iTokenParam;
			if constexpr (std::is_same_v<Return, coro::Task>) co_await stChild;
			else iRetcode = co_await stChild;
		}
	}
	catch (::OI::Base::ParserException&) { iRetcode = OI_ERR_FAIL; }
	catch (...)
	{
		iRetcode = OI_ERR_FAIL;
		coro::ReportTaskError(iTokenParam, std::current_exception());
	}
	::OI::Base::BytesStream stOutput;
	stOutput << iCallIdLocal << iRetcode;
	if (iRetcode == 0) stResponseLocal.serialize(stOutput);
	::OI::Base::BytesStream stFrame = EncodePbFrame(iResponseIdParam, stOutput);
	if (!pManagerParam->SendBytes(iSessionIdParam, stFrame.GetMutableBuff())) pManagerParam->CloseSession(iSessionIdParam);
	co_return;
}

template<typename Req, typename Res, typename F>
void HandleRpc(F fnFunctionParam)
{
	const proto_type_t iResponseIdLocal = RpcProtoId<Res>::iValue;
	BindRpcHandler(RpcProtoId<Req>::iValue, [fnFunctionParam, iResponseIdLocal](coro::Token iTokenParam, ::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, ::OI::Base::BytesStream stBodyParam) -> coro::Task {
		return RunHandledRpc<Req, Res>(fnFunctionParam, iResponseIdLocal, iTokenParam, pManagerParam, iSessionIdParam, std::move(stBodyParam));
	});
}

// 请求类型已关联响应，无需在注册处重复指定。
template<typename Req, typename F>
void HandleRpc(F fnFunctionParam)
{
	HandleRpc<Req, typename RpcProtoId<Req>::Response>(std::move(fnFunctionParam));
}

// 返回调用号。回包、超时或 WakeRpcCall 三者只有一个会触发 decode/resume。
uint64_t BindRpcCall(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, proto_type_t iResponseIdParam, std::coroutine_handle<> stHandleParam, int32_t* pResultParam, int64_t iTimeoutMillisecondsParam, std::function<bool(::OI::Base::BytesStream&)> fnDecodeParam, std::function<void()> fnResumeParam);
void FailRpcCalls(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam);
bool WakeRpcCall(uint64_t iCallIdParam, int32_t iCodeParam, ::OI::Base::BytesStream* pBodyParam);

// 发出请求后挂起。回包或超时回调里 Schedule 回来。回包可能比挂起先到。
template<typename Req, typename Res>
struct [[nodiscard]] RpcCallAwaiter
{
	static_assert(RpcProtoId<Req>::iValue == RpcProtoId<Res>::iValue, "RPC request/response protocol mismatch");
	::OI::Net::SessionManager* pManager;
	sid_t iSessionId;
	Req* pRequest;
	Res* pResponse;
	int32_t iResult;
	int64_t iTimeoutMilliseconds;
	std::coroutine_handle<> stHandle;
	std::atomic<int32_t> iGate;
	// 回包原样留到 await_resume 再解，解包跑在工人上，不占 IO 线程。
	::OI::Base::BytesStream stReply;
	bool bReplied;
	std::exception_ptr stSubmissionError;

	RpcCallAwaiter()
		: pManager(NULL), iSessionId(0), pRequest(NULL), pResponse(NULL), iResult(OI_ERR_FAIL), iTimeoutMilliseconds(180000), stHandle(), iGate(0), bReplied(false)
	{}
	RpcCallAwaiter(const RpcCallAwaiter&) = delete;
	RpcCallAwaiter(RpcCallAwaiter&& stOtherParam) noexcept
		: pManager(stOtherParam.pManager), iSessionId(stOtherParam.iSessionId), pRequest(stOtherParam.pRequest), pResponse(stOtherParam.pResponse), iResult(stOtherParam.iResult),
		  iTimeoutMilliseconds(stOtherParam.iTimeoutMilliseconds), stHandle(stOtherParam.stHandle), iGate(stOtherParam.iGate.load(std::memory_order_relaxed)),
		  bReplied(false), stSubmissionError(std::move(stOtherParam.stSubmissionError))
	{
		stOtherParam.stHandle = nullptr;
	}

	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		stHandle = stHandleParam;
		iResult = OI_ERR_FAIL;
		bReplied = false;
		stSubmissionError = nullptr;
		iGate.store(0, std::memory_order_relaxed);
		uint64_t iCallIdLocal = BindRpcCall(pManager, iSessionId, RpcProtoId<Res>::iValue, stHandleParam, &iResult, iTimeoutMilliseconds, [this](::OI::Base::BytesStream& stInputParam) {
			stReply = std::move(stInputParam);
			bReplied = true;
			return true;
		}, [this]() {
			int iPrevious = iGate.exchange(1, std::memory_order_acq_rel);
			if (iPrevious == 2)
			{
				coro::Schedule(stHandle);
			}
		});
		try
		{
			::OI::Base::BytesStream stBody;
			stBody << (iCallIdLocal | (uint64_t)RPC_REQ_MASK);
			pRequest->serialize(stBody);
			::OI::Base::BytesStream stFrame = EncodePbFrame(RpcProtoId<Req>::iValue, stBody);
			if (pManager->TryPendCall(iCallIdLocal, stFrame.GetMutableBuff()))
			{
			}
			else if (!pManager->SendBytes(iSessionId, stFrame.GetMutableBuff()))
			{
				WakeRpcCall(iCallIdLocal, pManager->UnavailableCode(), NULL);
			}
		}
		catch (...)
		{
			// 不直接从 await_suspend 抛出：超时/断连可能已取走等待项，正在使用 this。
			// 先完成等待，再由同一 gate 确认回调已结束，最后在 await_resume 重抛。
			stSubmissionError = std::current_exception();
			WakeRpcCall(iCallIdLocal, OI_ERR_FAIL, nullptr);
		}
		int iPrevious = iGate.exchange(2, std::memory_order_acq_rel);
		return iPrevious != 1;
	}
	int32_t await_resume()
	{
		if (stSubmissionError) std::rethrow_exception(stSubmissionError);
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
			}
		}
		return iResult;
	}
};

template<typename Req, typename Res>
RpcCallAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req& stRequestParam, Res& stResponseParam, int32_t iTimeoutSecondsParam = 180)
{
	RpcCallAwaiter<Req, Res> stAwaiter;
	stAwaiter.pManager = &stManagerParam;
	stAwaiter.iSessionId = iSessionIdParam;
	stAwaiter.pRequest = &stRequestParam;
	stAwaiter.pResponse = &stResponseParam;
	stAwaiter.iResult = OI_ERR_FAIL;
	stAwaiter.iTimeoutMilliseconds = static_cast<int64_t>(iTimeoutSecondsParam) * 1000;
	stAwaiter.stHandle = {};
	return stAwaiter;
}

template<typename Req, typename Res>
RpcCallAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req& stRequestParam, Res& stResponseParam, std::chrono::milliseconds stTimeoutParam)
{
	auto stAwaiter = RpcCall(stManagerParam, iSessionIdParam, stRequestParam, stResponseParam);
	stAwaiter.iTimeoutMilliseconds = stTimeoutParam.count();
	return stAwaiter;
}

template<typename Res>
class [[nodiscard]] RpcResult
{
	int32_t iReturnCode;
	Res stResponse;
	void Check() const
	{
		if (iReturnCode != 0) throw std::logic_error("RPC failed: check GetRet() before GetRes()");
	}
public:
	RpcResult(int32_t iReturnCodeParam, Res stResponseParam) : iReturnCode(iReturnCodeParam), stResponse(std::move(stResponseParam)) {}
	int32_t GetRet() const noexcept { return iReturnCode; }
	explicit operator bool() const noexcept { return iReturnCode == 0; }
	Res& GetRes() & { Check(); return stResponse; }
	const Res& GetRes() const & { Check(); return stResponse; }
	Res GetRes() && { Check(); return std::move(stResponse); }
	Res GetRes() const && { Check(); return stResponse; }
};

// 输出引用仅在成功解包后更新，错误时保留调用方原值。
template<typename Req>
auto RpcCall(ProtoClientManager& stManagerParam, Req& stRequestParam, typename RpcProtoId<Req>::Response& stResponseParam,
	std::chrono::milliseconds stTimeoutParam = ::OI::Timer::Seconds(180))
{
	return RpcCall(stManagerParam, stManagerParam.ConnectedSid(), stRequestParam, stResponseParam, stTimeoutParam);
}

// 拥有请求和响应，应用不必跨挂起维护输出引用。
template<typename Req, typename Res>
struct [[nodiscard]] RpcValueAwaiter : RpcCallAwaiter<Req, Res>
{
	Req stRequest;
	Res stResponse{};
	RpcValueAwaiter(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam, std::chrono::milliseconds stTimeoutParam)
		: stRequest(std::move(stRequestParam))
	{
		this->pManager = &stManagerParam;
		this->iSessionId = iSessionIdParam;
		this->iTimeoutMilliseconds = stTimeoutParam.count();
	}
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		this->pRequest = &stRequest;
		this->pResponse = &stResponse;
		return RpcCallAwaiter<Req, Res>::await_suspend(stHandleParam);
	}
	RpcResult<Res> await_resume()
	{
		return {RpcCallAwaiter<Req, Res>::await_resume(), std::move(stResponse)};
	}
};

template<typename Res, typename Req>
RpcValueAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam, std::chrono::milliseconds stTimeoutParam = std::chrono::seconds(180))
{
	return RpcValueAwaiter<Req, Res>(stManagerParam, iSessionIdParam, std::move(stRequestParam), stTimeoutParam);
}
// 自动配对响应类型；保留显式 Res 和输出引用接口供旧代码使用。
template<typename Req>
auto RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam,
	std::chrono::milliseconds stTimeoutParam = ::OI::Timer::Seconds(180))
	-> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {stManagerParam, iSessionIdParam, std::move(stRequestParam), stTimeoutParam};
}

template<typename Req>
auto RpcCall(ProtoClientManager& stManagerParam, Req stRequestParam,
	std::chrono::milliseconds stTimeoutParam = ::OI::Timer::Seconds(180))
	-> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {stManagerParam, stManagerParam.ConnectedSid(), std::move(stRequestParam), stTimeoutParam};
}

}}

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
