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

namespace coro
{
// 协程 token。StartTask 生成并传进协程。
typedef uint64_t Token;
typedef Token TaskUid;

TaskUid AllocTaskUid();

// 挂起的协程放回运行队列。单线程进本线程队列，多线程进工人队列。RPC 回包和定时器都走这里。
void Schedule(std::coroutine_handle<> stHandleParam);

// 根协程跑完后不能在 final_suspend 里销毁自己。登记下来，等 resume 返回再释放。
void DetachFinished(std::coroutine_handle<> stHandleParam, void (*fnReleaseParam)(std::coroutine_handle<>));
void DrainDetached();
// 调度器唯一的 resume 入口。期间新建的任务当场跑到第一次挂起，协程外新建的任务入队。
void ResumeTask(std::coroutine_handle<> stHandleParam);
bool InTaskResume() noexcept;

// 新任务构造 promise 时取走。只在 StartTask、TaskScope 等框架入口里设置。
struct TaskLaunch
{
	Token iToken = 0;
	bool bDetached = false;
	bool bPost = false;
};
TaskLaunch TakeTaskLaunch() noexcept;
class TaskLaunchScope
{
	TaskLaunch m_stPrevious;

  public:
	explicit TaskLaunchScope(TaskLaunch stLaunchParam) noexcept;
	~TaskLaunchScope();
	TaskLaunchScope(const TaskLaunchScope&) = delete;
	TaskLaunchScope& operator=(const TaskLaunchScope&) = delete;
};
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
	bool await_ready() const noexcept
	{
		return false;
	}
	void await_suspend(std::coroutine_handle<> stHandleParam) const noexcept;
	void await_resume() const noexcept
	{
	}
};

inline SleepMs yield(int32_t iMillisecondsParam)
{
	return SleepMs{iMillisecondsParam};
}

SleepMs yield(::OI::Timer::MS stDelayParam);

// 等待节点放在调用方协程帧中，单个或多个等待者都不另分配堆节点。
struct TaskWaiter
{
	std::coroutine_handle<> stParent;
	TaskWaiter* pNext = nullptr;
};
// 协程里新建的任务当场跑；协程外（网络回调、main）新建的或框架要求入队的，放进运行队列。
struct TaskInitialSuspend
{
	bool bInline;
	bool await_ready() const noexcept
	{
		return bInline;
	}
	void await_suspend(std::coroutine_handle<> stHandleParam) const noexcept
	{
		Schedule(stHandleParam);
	}
	void await_resume() const noexcept
	{
	}
};
struct TaskFinalSuspend
{
	bool await_ready() const noexcept
	{
		return false;
	}
	template <typename Promise>
	void await_suspend(std::coroutine_handle<Promise> stHandleParam) const noexcept
	{
		auto& stPromise = stHandleParam.promise();
		TaskWaiter* pWaiter;
		{
			OI::Thread::MutexGuard stLock(stPromise.stMutex);
			stPromise.bReady = true;
			pWaiter = std::exchange(stPromise.pWaiters, nullptr);
		}
		while (pWaiter)
		{
			// 调度后等待者可能立即恢复并销毁节点，只能再用局部副本。
			auto* pNext = pWaiter->pNext;
			Schedule(pWaiter->stParent);
			pWaiter = pNext;
		}
		// 执行引用必须等 resume 返回后才释放，不能在 final_suspend 中销毁帧。
		DetachFinished(stHandleParam, [](std::coroutine_handle<> stHandleLocal)
		{
			Promise::Release(std::coroutine_handle<Promise>::from_address(stHandleLocal.address()));
		});
	}
	void await_resume() const noexcept
	{
	}
};
struct TaskState
{
	std::shared_ptr<void> pCallable;
	std::exception_ptr stError;
	Token iTaskId = 0;
	OI::Thread::Mutex stMutex;
	TaskWaiter* pWaiters = nullptr;
	std::atomic<size_t> iReferences{2}; // Task 对象、等待者、运行中的帧各自持有引用。创建时即在运行。
	bool bReady = false;
	bool bInlineStart = false;
	std::atomic<bool> bDetached{false};
	std::atomic<bool> bObserved{false};
	std::atomic<bool> bErrorReported{false};
	TaskState()
	{
		const TaskLaunch stLaunch = TakeTaskLaunch();
		iTaskId = stLaunch.iToken;
		bDetached.store(stLaunch.bDetached, std::memory_order_relaxed);
		bInlineStart = !stLaunch.bPost && InTaskResume();
	}
	TaskInitialSuspend initial_suspend() noexcept
	{
		return {bInlineStart};
	}
	TaskFinalSuspend final_suspend() noexcept
	{
		return {};
	}
	void unhandled_exception() noexcept
	{
		stError = std::current_exception();
		if (bDetached.load(std::memory_order_acquire))
		{
			ReportError();
		}
	}
	void ReportError() noexcept
	{
		if (!bErrorReported.exchange(true, std::memory_order_acq_rel))
		{
			ReportTaskError(iTaskId, stError);
		}
	}
};

// 有返回值和无返回值任务共用同一套调度与完成门，不增加包装协程。
template <typename T>
struct TaskReturn
{
	std::optional<T> stResult;
	void return_value(T stValueParam)
	{
		stResult.emplace(std::move(stValueParam));
	}
};
template <>
struct TaskReturn<void>
{
	void return_void() noexcept
	{
	}
};

// 一条任务只运行一次，可多次/并发等待；结果和异常保留到最后一个引用释放。
template <typename T = void>
struct BasicTask
{
	struct promise_type : TaskState, TaskReturn<T>
	{
		BasicTask get_return_object()
		{
			return BasicTask{std::coroutine_handle<promise_type>::from_promise(*this)};
		}
		static void Release(std::coroutine_handle<promise_type> stHandleParam)
		{
			if (stHandleParam && stHandleParam.promise().iReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
			{
				// 没有人取走的异常在最后一个引用释放时报告，不会悄悄丢掉。
				auto& stPromise = stHandleParam.promise();
				if (stPromise.stError && !stPromise.bObserved.load(std::memory_order_acquire))
				{
					stPromise.ReportError();
				}
				stHandleParam.destroy();
			}
		}
	};
	using Handle = std::coroutine_handle<promise_type>;
	Handle stHandle;
	explicit BasicTask(Handle stHandleParam)
	    : stHandle(stHandleParam)
	{
	}
	BasicTask(BasicTask&& stOtherParam) noexcept
	    : stHandle(std::exchange(stOtherParam.stHandle, {}))
	{
	}
	// 外层只是共享句柄，复制不复制协程帧，也不会再次执行任务。
	BasicTask(const BasicTask& stOtherParam) noexcept
	    : stHandle(stOtherParam.stHandle)
	{
		if (stHandle)
		{
			stHandle.promise().iReferences.fetch_add(1, std::memory_order_relaxed);
		}
	}
	BasicTask& operator=(const BasicTask& stOtherParam) noexcept
	{
		BasicTask stCopy(stOtherParam);
		std::swap(stHandle, stCopy.stHandle);
		return *this;
	}
	BasicTask& operator=(BasicTask&& stOtherParam) noexcept
	{
		if (this != &stOtherParam)
		{
			BasicTask stMoved(std::move(stOtherParam));
			std::swap(stHandle, stMoved.stHandle);
		}
		return *this;
	}
	~BasicTask()
	{
		promise_type::Release(stHandle);
	}
	Token GetToken() const
	{
		if (!stHandle)
		{
			return 0;
		}
		auto& stPromise = stHandle.promise();
		OI::Thread::MutexGuard stLock(stPromise.stMutex);
		return stPromise.iTaskId;
	}

	// 任务创建即运行。Start 只把它标成独立任务：分配 token、保活闭包、异常直接报告。可重复调用。
	Token Start(std::shared_ptr<void> pCallableParam = {}, Token iTokenParam = 0) const
	{
		if (!stHandle)
		{
			throw std::logic_error("cannot start a moved-from Task");
		}
		auto& stPromise = stHandle.promise();
		OI::Thread::MutexGuard stLock(stPromise.stMutex);
		if (stPromise.iTaskId == 0)
		{
			stPromise.iTaskId = iTokenParam ? iTokenParam : AllocTaskUid();
		}
		if (!stPromise.pCallable)
		{
			stPromise.pCallable = std::move(pCallableParam);
		}
		if (!stPromise.bDetached.exchange(true, std::memory_order_acq_rel) && stPromise.bReady && stPromise.stError)
		{
			stPromise.ReportError();
		}
		return stPromise.iTaskId;
	}

	struct Awaiter : TaskWaiter
	{
		Handle stChild;
		explicit Awaiter(Handle stChildParam)
		    : stChild(stChildParam)
		{
			if (!stChild)
			{
				throw std::logic_error("cannot await a moved-from Task");
			}
			stChild.promise().iReferences.fetch_add(1, std::memory_order_relaxed);
		}
		Awaiter(const Awaiter&) = delete;
		~Awaiter()
		{
			promise_type::Release(stChild);
		}
		bool await_ready()
		{
			// 当场跑完的子任务的执行引用在这里放，不等最外层协程挂起，避免帧堆在线程局部队列里。
			DrainDetached();
			auto& stPromise = stChild.promise();
			OI::Thread::MutexGuard stLock(stPromise.stMutex);
			return stPromise.bReady;
		}
		bool await_suspend(std::coroutine_handle<> stParentParam)
		{
			// 登记和完成共用锁。登记后子任务可能在别的线程立即完成并调度本协程，之后不再访问 this。
			auto& stPromise = stChild.promise();
			OI::Thread::MutexGuard stLock(stPromise.stMutex);
			if (stPromise.bReady)
			{
				return false;
			}
			stParent = stParentParam;
			pNext = stPromise.pWaiters;
			stPromise.pWaiters = this;
			return true;
		}
		T await_resume()
		{
			auto& stPromise = stChild.promise();
			if (stPromise.stError)
			{
				stPromise.bObserved.store(true, std::memory_order_release);
				std::rethrow_exception(stPromise.stError);
			}
			if constexpr (!std::is_void_v<T>)
			{
				static_assert(std::is_copy_constructible_v<T>, "shared TaskResult needs a copyable result; use shared_ptr<T> for move-only data");
				if (!stPromise.stResult)
				{
					throw std::logic_error("Task has no result");
				}
				return *stPromise.stResult; // 不移走共享结果，后续等待者仍得到完整值。
			}
		}
	};
	Awaiter operator co_await() const
	{
		return Awaiter(stHandle);
	}
};

using Task = BasicTask<void>;
template <typename T>
using TaskResult = BasicTask<T>;

// 在 stLaunchParam 下调用 fnFunctionParam，它新建的第一条任务取走这份启动信息。
template <typename F>
auto InvokeTask(TaskLaunch stLaunchParam, F&& fnFunctionParam)
{
	TaskLaunchScope stScope(stLaunchParam);
	return std::forward<F>(fnFunctionParam)();
}

// 闭包由任务保活，带捕获的协程 lambda 必须经这里启动。任务总是入队，不在调用方栈上跑。
template <typename F>
auto StartTask(F&& fnFunctionParam)
{
	Token iTokenLocal = AllocTaskUid();
	auto stCallable = std::make_shared<std::decay_t<F>>(std::forward<F>(fnFunctionParam));
	auto stTask = InvokeTask(TaskLaunch{iTokenLocal, true, true}, [&stCallable, iTokenLocal]()
	{
		return (*stCallable)(iTokenLocal);
	});
	stTask.Start(std::move(stCallable), iTokenLocal);
	return stTask;
}

// 协作停止：不会销毁仍被 IO/定时器引用的帧。模块销毁前必须 await Join。
class StopToken
{
	std::shared_ptr<std::atomic<bool>> m_pStop;

  public:
	explicit StopToken(std::shared_ptr<std::atomic<bool>> pStopParam)
	    : m_pStop(std::move(pStopParam))
	{
	}
	bool StopRequested() const
	{
		return m_pStop->load(std::memory_order_acquire);
	}
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
	template <typename F>
	static Task Run(std::shared_ptr<State> pStateParam, F fnFunctionParam, Token iTokenParam)
	{
		try
		{
			co_await fnFunctionParam(iTokenParam, StopToken(pStateParam->pStop));
		}
		catch (...)
		{
			::OI::Thread::MutexGuard stLock(pStateParam->stMutex);
			if (!pStateParam->stError)
			{
				pStateParam->stError = std::current_exception();
			}
			pStateParam->pStop->store(true, std::memory_order_release);
		}
		std::vector<std::coroutine_handle<>> vWaitersLocal;
		{
			::OI::Thread::MutexGuard stLock(pStateParam->stMutex);
			if (--pStateParam->iActive == 0)
			{
				vWaitersLocal.swap(pStateParam->vWaiters);
			}
		}
		for (const auto& stWaiter : vWaitersLocal)
		{
			Schedule(stWaiter);
		}
	}
	struct JoinAwaiter
	{
		std::shared_ptr<State> pState;
		bool await_ready() const noexcept
		{
			return false;
		}
		bool await_suspend(std::coroutine_handle<> stWaiterParam);
		void await_resume();
	};
	static Task Wait(std::shared_ptr<State> pStateParam);

  public:
	TaskScope() = default;
	TaskScope(const TaskScope&) = delete;
	TaskScope& operator=(const TaskScope&) = delete;
	~TaskScope();
	void RequestStop();
	template <typename F>
	Token Start(F&& fnFunctionParam)
	{
		::OI::Thread::MutexGuard stLock(m_pState->stMutex);
		if (m_pState->bClosed)
		{
			throw std::logic_error("TaskScope is closed");
		}
		Token iTokenLocal = AllocTaskUid();
		// 任务入队后要等这把锁才能结束，计数晚一步加不会被提前减到零。
		auto stTask = InvokeTask(TaskLaunch{iTokenLocal, true, true}, [this, &fnFunctionParam, iTokenLocal]()
		{
			return Run(m_pState, std::forward<F>(fnFunctionParam), iTokenLocal);
		});
		++m_pState->iActive;
		return stTask.Start();
	}
	Task Join();
};

}

namespace OI
{
namespace Rpc
{
// 标准服务端协程：co_return 0 成功，正数为业务错误。
using RpcTask = coro::TaskResult<int32_t>;

// 仅需要连接信息的处理函数接收上下文；生命周期覆盖本次处理协程。
struct RpcContext
{
	coro::Token iToken;
	::OI::Net::SessionManager& stManager;
	sid_t iSessionId;
};

template <typename T>
struct RpcProtoId;

void BindRpcHandler(proto_type_t iIdParam, std::function<coro::Task(coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> fnHandleParam);

// 请求和回包共用一个协议号。调用号最高位是 1 表示请求，回包清掉这一位。
// 请求：调用号 + Req；回执：调用号 + int32_t retcode + 成功时的 Res。
// 0 成功，负数保留给框架错误，正数用于业务错误。失败不序列化 Res。

// 业务任务外面这一层：解包，co_await 业务任务，再把带调用号的回包写出去。
template <typename Req, typename Res, typename F>
coro::Task RunHandledRpc(F fnFunctionParam, proto_type_t iResponseIdParam, coro::Token iTokenParam, ::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, ::OI::Base::BytesStream stBodyParam)
{
	uint64_t iCallIdLocal = 0;
	try
	{
		stBodyParam >> iCallIdLocal;
	}
	catch (::OI::Base::ParserException&)
	{
		co_return;
	}
	iCallIdLocal &= ~(uint64_t)RPC_REQ_MASK;
	Req stRequestLocal{};
	Res stResponseLocal{};
	int32_t iRetcode = 0;
	try
	{
		stRequestLocal.deserialize(stBodyParam);
		RpcContext stContext{iTokenParam, *pManagerParam, iSessionIdParam};
		auto stInvoke = [pFunction = &fnFunctionParam, pRequest = &stRequestLocal, pResponse = &stResponseLocal, pContext = &stContext]()
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
			iRetcode = co_await coro::InvokeTask(coro::TaskLaunch{iTokenParam, false, false}, stInvoke);
		}
	}
	catch (::OI::Base::ParserException&)
	{
		iRetcode = OI_ERR_FAIL;
	}
	catch (...)
	{
		iRetcode = OI_ERR_FAIL;
		coro::ReportTaskError(iTokenParam, std::current_exception());
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
		stFrame = EncodePbFrame(iResponseIdParam, stOutput);
	}
	catch (...)
	{
		coro::ReportTaskError(iTokenParam, std::current_exception());
		// 丢弃已经写入的半包，重新构造只有调用号和错误码的回执。
		try
		{
			::OI::Base::BytesStream stFailure;
			stFailure << iCallIdLocal << int32_t(OI_ERR_FAIL);
			stFrame = EncodePbFrame(iResponseIdParam, stFailure);
		}
		catch (...)
		{
			coro::ReportTaskError(iTokenParam, std::current_exception());
			pManagerParam->CloseSession(iSessionIdParam);
			co_return;
		}
	}
	try
	{
		if (!pManagerParam->SendBytes(iSessionIdParam, stFrame.GetMutableBuff()))
		{
			pManagerParam->CloseSession(iSessionIdParam);
		}
	}
	catch (...)
	{
		coro::ReportTaskError(iTokenParam, std::current_exception());
		pManagerParam->CloseSession(iSessionIdParam);
	}
	co_return;
}

template <typename Req, typename Res, typename F>
void HandleRpc(F fnFunctionParam)
{
	const proto_type_t iResponseIdLocal = RpcProtoId<Res>::iValue;
	BindRpcHandler(RpcProtoId<Req>::iValue, [fnFunctionParam, iResponseIdLocal](coro::Token iTokenParam, ::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, ::OI::Base::BytesStream stBodyParam) -> coro::Task
	{
		return RunHandledRpc<Req, Res>(fnFunctionParam, iResponseIdLocal, iTokenParam, pManagerParam, iSessionIdParam, std::move(stBodyParam));
	});
}

// 请求类型已关联响应，无需在注册处重复指定。
template <typename Req, typename F>
void HandleRpc(F fnFunctionParam)
{
	HandleRpc<Req, typename RpcProtoId<Req>::Response>(std::move(fnFunctionParam));
}

// 返回调用号。回包、超时或 WakeRpcCall 三者只有一个会触发 decode/resume。
uint64_t BindRpcCall(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, proto_type_t iResponseIdParam, std::coroutine_handle<> stHandleParam, int32_t* pResultParam, int64_t iTimeoutMillisecondsParam, std::function<bool(::OI::Base::BytesStream&)> fnDecodeParam, std::function<void()> fnResumeParam, std::shared_ptr<std::atomic<int32_t>> pGuardParam = {});
void FailRpcCalls(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam);
// 取走等待项且不调用完成回调。已经在回调里的，等 WaitRpcGuard 归零。
void AbandonRpcCall(uint64_t iCallIdParam);
void WaitRpcGuard(const std::shared_ptr<std::atomic<int32_t>>& pGuardParam);
bool WakeRpcCall(uint64_t iCallIdParam, int32_t iCodeParam, ::OI::Base::BytesStream* pBodyParam);

// 发出请求后挂起。回包或超时回调里 Schedule 回来。回包可能比挂起先到。
template <typename Req, typename Res>
struct [[nodiscard]] RpcCallAwaiter
{
	static_assert(RpcProtoId<Req>::iValue == RpcProtoId<Res>::iValue, "RPC request/response protocol mismatch");
	::OI::Net::SessionManager* pManager;
	ProtoClientManager* pClient = nullptr; // 便捷调用在提交时选择会话，显式 sid 调用仍固定会话。
	sid_t iSessionId;
	Req* pRequest;
	Res* pResponse;
	int32_t iResult;
	int64_t iTimeoutMilliseconds;
	std::coroutine_handle<> stHandle;
	std::atomic<int32_t> iGate;
	std::atomic<bool> bAwaitStarted{false};
	bool bDispatched = false;
	// 回包原样留到 await_resume 再解，解包跑在工人上，不占 IO 线程。
	::OI::Base::BytesStream stReply;
	bool bReplied;
	std::exception_ptr stSubmissionError;
	uint64_t iCallId = 0;
	std::shared_ptr<std::atomic<int32_t>> pGuard;

	RpcCallAwaiter()
	    : pManager(NULL), iSessionId(0), pRequest(NULL), pResponse(NULL), iResult(OI_ERR_FAIL), iTimeoutMilliseconds(180000), stHandle(), iGate(0), bReplied(false)
	{
	}
	RpcCallAwaiter(const RpcCallAwaiter&) = delete;
	RpcCallAwaiter(RpcCallAwaiter&& stOtherParam)
	    : pManager(ClaimMove(stOtherParam)), pClient(stOtherParam.pClient), iSessionId(stOtherParam.iSessionId), pRequest(stOtherParam.pRequest), pResponse(stOtherParam.pResponse), iResult(stOtherParam.iResult),
	      iTimeoutMilliseconds(stOtherParam.iTimeoutMilliseconds), stHandle(stOtherParam.stHandle), iGate(stOtherParam.iGate.load(std::memory_order_relaxed)),
	      bReplied(false), stSubmissionError(std::move(stOtherParam.stSubmissionError)), iCallId(stOtherParam.iCallId), pGuard(std::move(stOtherParam.pGuard))
	{
		stOtherParam.stHandle = nullptr;
		stOtherParam.iCallId = 0;
		stOtherParam.bDispatched = false;
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
	static ::OI::Net::SessionManager* ClaimMove(RpcCallAwaiter& stOtherParam)
	{
		stOtherParam.ClaimAwait();
		if (stOtherParam.bDispatched)
		{
			throw std::logic_error("RpcCall can only be awaited once; cannot reuse or move a submitted call");
		}
		return stOtherParam.pManager;
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
	bool await_suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		ClaimAwait();
		return Suspend(stHandleParam);
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
		iGate.store(0, std::memory_order_relaxed);
		iCallId = 0;
		pGuard = std::make_shared<std::atomic<int32_t>>(0);
		uint64_t iCallIdLocal = BindRpcCall(pManager, iSessionId, RpcProtoId<Res>::iValue, stHandle, &iResult, iTimeoutMilliseconds, [this](::OI::Base::BytesStream& stInputParam)
		{
			stReply = std::move(stInputParam);
			bReplied = true;
			return true;
		}, [this]()
		{
			int iPrevious = iGate.exchange(1, std::memory_order_acq_rel);
			if (iPrevious == 2)
			{
				coro::Schedule(stHandle);
			}
		}, pGuard);
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
	}
	bool Arm(std::coroutine_handle<> stHandleParam)
	{
		stHandle = stHandleParam;
		int iPrevious = iGate.exchange(2, std::memory_order_acq_rel);
		return iPrevious != 1;
	}
	template <typename Promise>
	bool Suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		Dispatch();
		return Arm(stHandleParam);
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
			}
		}
		return iResult;
	}
};

template <typename Req, typename Res>
RpcCallAwaiter<Req, Res> MakeRpcCallAwaiter(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req& stRequestParam, Res& stResponseParam, ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
{
	RpcCallAwaiter<Req, Res> stAwaiter;
	stAwaiter.pManager = &stManagerParam;
	stAwaiter.iSessionId = iSessionIdParam;
	stAwaiter.pRequest = &stRequestParam;
	stAwaiter.pResponse = &stResponseParam;
	stAwaiter.iTimeoutMilliseconds = stTimeoutParam.iMilliseconds;
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
	RpcResult(int32_t iReturnCodeParam, Res stResponseParam)
	    : m_iReturnCode(iReturnCodeParam), m_stResponse(std::move(stResponseParam))
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
auto MakeRpcCallAwaiter(ProtoClientManager& stManagerParam, Req& stRequestParam, typename RpcProtoId<Req>::Response& stResponseParam,
    ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
{
	auto stCall = MakeRpcCallAwaiter(stManagerParam, INVALID_SID, stRequestParam, stResponseParam, stTimeoutParam);
	stCall.pClient = &stManagerParam;
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
coro::TaskResult<int32_t> RunOutputCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam,
    ProtoClientManager* pClientParam, std::optional<Req> stRequestParam,
    typename RpcProtoId<Req>::Response& stResponseParam, ::OI::Timer::MS stTimeoutParam)
{
	RpcRequestRelease<Req> stRelease{stRequestParam};
	auto stCall = MakeRpcCallAwaiter(stManagerParam, iSessionIdParam, *stRequestParam, stResponseParam, stTimeoutParam);
	stCall.pClient = pClientParam;
	co_return co_await stCall;
}

// 普通函数在返回前把请求移入任务；响应必须活到任务结束。
template <typename Req>
[[nodiscard]] coro::TaskResult<int32_t> RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam,
    Req stRequestParam, typename RpcProtoId<Req>::Response& stResponseParam,
    ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
{
	return RunOutputCall<Req>(stManagerParam, iSessionIdParam, nullptr,
	    std::optional<Req>(std::in_place, std::move(stRequestParam)), stResponseParam, stTimeoutParam);
}

template <typename Req>
[[nodiscard]] coro::TaskResult<int32_t> RpcCall(ProtoClientManager& stManagerParam, Req stRequestParam,
    typename RpcProtoId<Req>::Response& stResponseParam, ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
{
	return RunOutputCall<Req>(stManagerParam, INVALID_SID, &stManagerParam,
	    std::optional<Req>(std::in_place, std::move(stRequestParam)), stResponseParam, stTimeoutParam);
}

template <typename Req, typename Res>
struct [[nodiscard]] RpcValueAwaiter : RpcCallAwaiter<Req, Res>
{
	Req stRequest;
	Res stResponse{};
	RpcValueAwaiter(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam, ::OI::Timer::MS stTimeoutParam, ProtoClientManager* pClientParam = nullptr)
	    : stRequest(std::move(stRequestParam))
	{
		this->pManager = &stManagerParam;
		this->pClient = pClientParam;
		this->iSessionId = iSessionIdParam;
		this->iTimeoutMilliseconds = stTimeoutParam.iMilliseconds;
	}
	void Submit()
	{
		this->pRequest = &stRequest;
		this->pResponse = &stResponse;
		this->Dispatch();
	}
	template <typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		// 先独占等待权，再决定要不要发送。并发等待必须在改完成门之前被拒绝。
		this->ClaimAwait();
		if (!this->bDispatched)
		{
			this->pRequest = &stRequest;
			this->pResponse = &stResponse;
			this->Dispatch();
		}
		return this->Arm(stHandleParam);
	}
	RpcResult<Res> await_resume()
	{
		// 解包会写入 stResponse，必须完成后再移动响应，明确跨编译器求值顺序。
		const int32_t iCode = RpcCallAwaiter<Req, Res>::await_resume();
		return {iCode, std::move(stResponse)};
	}
};

template <typename Res, typename Req>
RpcValueAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam, ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
{
	return RpcValueAwaiter<Req, Res>(stManagerParam, iSessionIdParam, std::move(stRequestParam), stTimeoutParam);
}
// 自动配对响应类型；保留显式 Res 和输出引用接口供旧代码使用。
template <typename Req>
auto RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam,
    ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
    -> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {stManagerParam, iSessionIdParam, std::move(stRequestParam), stTimeoutParam};
}

template <typename Req>
auto RpcCall(ProtoClientManager& stManagerParam, Req stRequestParam,
    ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
    -> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {stManagerParam, INVALID_SID, std::move(stRequestParam), stTimeoutParam, &stManagerParam};
}

// 同一调用的查询、建连和回包共享单调时钟预算。非正值保持无限 RPC 等待语义。
struct RpcDeadline
{
	int64_t iDeadlineMilliseconds = 0;
	RpcDeadline() = default;
	RpcDeadline(::OI::Timer::MS stTimeoutParam)
	{
		if (stTimeoutParam.iMilliseconds > 0)
		{
			const int64_t iNow = ::OI::Timer::SteadyMilliseconds();
			iDeadlineMilliseconds = stTimeoutParam.iMilliseconds > INT64_MAX - iNow ? INT64_MAX : iNow + stTimeoutParam.iMilliseconds;
		}
	}
	template <typename Duration, std::enable_if_t<std::is_convertible_v<Duration, ::OI::Timer::MS>, int> = 0>
	RpcDeadline(Duration stTimeoutParam)
	    : RpcDeadline(::OI::Timer::MS(stTimeoutParam))
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
	std::vector<ProtoClientManager*> vClients;
	std::vector<std::shared_ptr<ProtoClientManager>> vOwners;
};

inline int32_t RouteError(ServiceRouteStatus iStatusParam)
{
	switch (iStatusParam)
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

// pManagerParam 是发起调用的管理器。不带分区且它自己就在看这个名字时，直接用它，不再另连。
// 返回 true 表示不用再问注册表，结果已经写进 stRouteParam。
bool PrepareRoute(::OI::Net::SessionManager* pManagerParam, const std::string& sNameParam, ServicePick iPickParam, const std::string& sZoneParam, ServiceRoute& stRouteParam);
bool PrepareZones(::OI::Net::SessionManager* pManagerParam, const std::string& sNameParam, const std::vector<int32_t>& vZoneIdsParam, ServiceRoute& stRouteParam, std::vector<std::string>& vZonesParam);
// bBatchParam 为真时只接受 name.*，否则只接受 name 或 name.分区。
// 返回 true 表示目标字符串非法，结果在 stRouteParam。否则写出要拉取的名字、挑选方式和分区。
bool PrepareTarget(const std::string& sTargetParam, bool bBatchParam, ServiceRoute& stRouteParam, std::string& sNameParam, ServicePick& iPickParam, std::string& sZoneParam);
// 拉取不占线程。fnDone 只调用一次，里面不能再等注册表。
void PullRoute(const std::string& sNameParam, std::function<void(int32_t, const std::vector<::OI::Base::ServiceInstance>&)> fnDoneParam, int64_t iTimeoutMillisecondsParam = 1000);
// 名单完成后的路由只引用固定地址的池连接；不读取或复用可能切换地址的订阅管理器。
ServiceRoute FinishRoute(const std::string& sNameParam, ServicePick iPickParam, const std::string& sZoneParam, int32_t iCodeParam, const std::vector<::OI::Base::ServiceInstance>& vListParam, ::OI::Net::SessionManager* pManagerParam = nullptr);
// vClients 与分区号等长，空指针表示这个分区当前没登记。
ServiceRoute FinishZones(const std::string& sNameParam, const std::vector<std::string>& vZonesParam, int32_t iCodeParam, const std::vector<::OI::Base::ServiceInstance>& vListParam, ::OI::Net::SessionManager* pManagerParam = nullptr);

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
	int32_t iCode = OI_ERR_CONNECT;
	std::vector<::OI::Base::ServiceInstance> vList;
	std::vector<std::string> vZoneNames;
	ServiceRoute stRoute;
	std::coroutine_handle<> stHandle{};
	std::atomic<int32_t> iGate{0};

	bool await_ready() const noexcept
	{
		return false;
	}
	template <typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		stHandle = stHandleParam;
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
		PullRoute(sName, [this](int32_t iCodeParam, const std::vector<::OI::Base::ServiceInstance>& vListParam)
		{
			iCode = iCodeParam;
			try
			{
				vList = vListParam;
			}
			catch (...)
			{
				iCode = OI_ERR_FAIL;
			}
			if (iGate.exchange(1, std::memory_order_acq_rel) == 2)
			{
				coro::Schedule(stHandle);
			}
		}, iRemaining > 0 ? (std::min)(iRemaining, int64_t(1000)) : 1000);
		return iGate.exchange(2, std::memory_order_acq_rel) != 1;
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
coro::TaskResult<RpcResult<Res>> FailNamed(int32_t iCodeParam)
{
	co_return RpcResult<Res>(iCodeParam, Res{});
}

template <typename Res, typename Req>
coro::TaskResult<RpcResult<Res>> CallNamed(::OI::Net::SessionManager* pManagerParam, std::string sNameParam, ServicePick iPickParam, std::string sZoneParam, Req stRequestParam, RpcDeadline stDeadlineParam)
{
	RouteAwaiter stWait;
	stWait.stDeadline = stDeadlineParam;
	stWait.iKind = RouteAwaiter::Kind::Named;
	stWait.pManager = pManagerParam;
	stWait.sName = std::move(sNameParam);
	stWait.iPick = iPickParam;
	stWait.sZone = std::move(sZoneParam);
	ServiceRoute stRoute = co_await stWait;
	if (stRoute.iStatus != ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
	{
		int32_t iCode = RouteError(stRoute.iStatus);
		co_return RpcResult<Res>(iCode, Res{});
	}
	if (stDeadlineParam.Expired())
	{
		co_return RpcResult<Res>(OI_ERR_TIMEOUT, Res{});
	}
	auto stCall = RpcCall(*stRoute.vClients[0], std::move(stRequestParam), stDeadlineParam.Remaining());
	co_return co_await stCall;
}

template <typename Res, typename Req>
auto ProtoClientManager::CallAny(Req stRequestParam, ::OI::Timer::MS stTimeoutParam)
{
	static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
	return CallNamed<Res>(this, sServiceName, ServicePick::Any, std::string(), std::move(stRequestParam), stTimeoutParam);
}

template <typename Res, typename Req>
auto ProtoClientManager::Call(int32_t iZoneIdParam, Req stRequestParam, ::OI::Timer::MS stTimeoutParam)
{
	static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
	if (iZoneIdParam < 0)
	{
		return FailNamed<Res, Req>(OI_ERR_FAIL);
	}
	return CallNamed<Res>(this, sServiceName, ServicePick::Zone, std::to_string(iZoneIdParam), std::move(stRequestParam), stTimeoutParam);
}

// 命名调用与直连共用同一个解包等待器，响应不经过 RpcResult 中转或复制。
template <typename Req>
coro::TaskResult<int32_t> RunNamedOutput(::OI::Net::SessionManager* pManagerParam, std::string sNameParam,
    ServicePick iPickParam, std::string sZoneParam, std::optional<Req> stRequestParam,
    typename RpcProtoId<Req>::Response& stResponseParam, RpcDeadline stDeadlineParam)
{
	RpcRequestRelease<Req> stRelease{stRequestParam};
	RouteAwaiter stWait;
	stWait.stDeadline = stDeadlineParam;
	stWait.pManager = pManagerParam;
	stWait.sName = std::move(sNameParam);
	stWait.iPick = iPickParam;
	stWait.sZone = std::move(sZoneParam);
	ServiceRoute stRoute = co_await stWait;
	if (stRoute.iStatus != ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
	{
		co_return RouteError(stRoute.iStatus);
	}
	if (stDeadlineParam.Expired())
	{
		co_return OI_ERR_TIMEOUT;
	}
	auto stCall = MakeRpcCallAwaiter(*stRoute.vClients[0], *stRequestParam, stResponseParam, stDeadlineParam.Remaining());
	co_return co_await stCall;
}
template <typename Req>
coro::TaskResult<int32_t> CallNamedOutput(::OI::Net::SessionManager* pManagerParam, std::string sNameParam,
    ServicePick iPickParam, std::string sZoneParam, Req stRequestParam,
    typename RpcProtoId<Req>::Response& stResponseParam, RpcDeadline stDeadlineParam)
{
	return RunNamedOutput<Req>(pManagerParam, std::move(sNameParam), iPickParam, std::move(sZoneParam),
	    std::optional<Req>(std::in_place, std::move(stRequestParam)), stResponseParam, stDeadlineParam);
}

inline coro::TaskResult<int32_t> FailRpcOutput(int32_t iCodeParam)
{
	co_return iCodeParam;
}

template <typename Req>
[[nodiscard]] auto ProtoClientManager::Call(Req stRequestParam, typename RpcProtoId<Req>::Response& stResponseParam,
    ::OI::Timer::MS stTimeoutParam)
{
	// 不指定分区时沿用当前客户端的连接、首次建连排队和固定配置。
	return RpcCall(*this, std::move(stRequestParam), stResponseParam, stTimeoutParam);
}

template <typename Req>
[[nodiscard]] auto ProtoClientManager::CallAny(Req stRequestParam, typename RpcProtoId<Req>::Response& stResponseParam,
    ::OI::Timer::MS stTimeoutParam)
{
	return CallNamedOutput(this, sServiceName, ServicePick::Any, std::string(), std::move(stRequestParam), stResponseParam, stTimeoutParam);
}

template <typename Req>
[[nodiscard]] auto ProtoClientManager::Call(int32_t iZoneIdParam, Req stRequestParam,
    typename RpcProtoId<Req>::Response& stResponseParam, ::OI::Timer::MS stTimeoutParam)
{
	if (iZoneIdParam < 0)
	{
		return FailRpcOutput(OI_ERR_FAIL);
	}
	return CallNamedOutput(this, sServiceName, ServicePick::Zone, std::to_string(iZoneIdParam), std::move(stRequestParam), stResponseParam, stTimeoutParam);
}

template <typename Req>
[[nodiscard]] coro::TaskResult<int32_t> RpcCall(const std::string& sServiceParam, Req stRequestParam,
    typename RpcProtoId<Req>::Response& stResponseParam, ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
{
	// 本函数不是协程，解析/拷贝在返回前完成；任务帧按值保存选路参数。
	ServiceRoute stRoute;
	std::string sName;
	std::string sZone;
	ServicePick iPick = ServicePick::Any;
	if (PrepareTarget(sServiceParam, false, stRoute, sName, iPick, sZone))
	{
		return FailRpcOutput(OI_ERR_FAIL);
	}
	return CallNamedOutput(nullptr, std::move(sName), iPick, std::move(sZone), std::move(stRequestParam), stResponseParam, stTimeoutParam);
}

template <typename Res, typename Req>
coro::TaskResult<std::vector<RpcResult<Res>>> SendRouted(ServiceRoute stRouteParam, Req stRequestParam, RpcDeadline stDeadlineParam)
{
	ServiceRoute stRoute = std::move(stRouteParam);
	static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
	if (stRoute.iStatus != ServiceRouteStatus::Ok)
	{
		int32_t iCode = RouteError(stRoute.iStatus);
		co_return std::vector<RpcResult<Res>>{RpcResult<Res>(iCode, Res{})};
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
			if (stDeadlineParam.Expired())
			{
				vSkipped[iCurrentSlot] = OI_ERR_TIMEOUT;
				vCalls.emplace_back();
				continue;
			}
			vCalls.emplace_back(std::in_place, RpcCall(*pClient, stRequestParam, stDeadlineParam.Remaining()));
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
coro::TaskResult<std::vector<RpcResult<Res>>> CallRouted(RouteAwaiter::Kind iKindParam, ::OI::Net::SessionManager* pManagerParam, std::string sNameParam, ServicePick iPickParam, std::string sZoneParam, std::vector<int32_t> vZonesParam, std::string sTargetParam, bool bBatchParam, Req stRequestParam, RpcDeadline stDeadlineParam)
{
	RouteAwaiter stWait;
	stWait.stDeadline = stDeadlineParam;
	stWait.iKind = iKindParam;
	stWait.pManager = pManagerParam;
	stWait.sName = std::move(sNameParam);
	stWait.iPick = iPickParam;
	stWait.sZone = std::move(sZoneParam);
	stWait.vZones = std::move(vZonesParam);
	stWait.sTarget = std::move(sTargetParam);
	stWait.bBatch = bBatchParam;
	ServiceRoute stRoute = co_await stWait;
	auto stBatch = SendRouted<Res>(std::move(stRoute), std::move(stRequestParam), stDeadlineParam);
	co_return co_await stBatch;
}

template <typename Res, typename Req>
coro::TaskResult<std::vector<RpcResult<Res>>> RpcCallBatch(const std::string& sServiceParam, Req stRequestParam, ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
{
	return CallRouted<Res>(RouteAwaiter::Kind::Target, nullptr, std::string(), ServicePick::Any, std::string(), {}, sServiceParam, true, std::move(stRequestParam), stTimeoutParam);
}

template <typename Res, typename Req>
coro::TaskResult<RpcResult<Res>> RunTargetCall(std::string sServiceParam, Req stRequestParam, RpcDeadline stDeadlineParam)
{
	static_assert(std::is_same_v<Res, typename RpcProtoId<Req>::Response>, "RPC response type does not match the request");
	RouteAwaiter stWait;
	stWait.stDeadline = stDeadlineParam;
	stWait.iKind = RouteAwaiter::Kind::Target;
	stWait.sTarget = std::move(sServiceParam);
	stWait.bBatch = false;
	ServiceRoute stRoute = co_await stWait;
	if (stRoute.iStatus != ServiceRouteStatus::Ok || stRoute.vClients.size() != 1 || !stRoute.vClients[0])
	{
		int32_t iCode = RouteError(stRoute.iStatus);
		co_return RpcResult<Res>(iCode, Res{});
	}
	if (stDeadlineParam.Expired())
	{
		co_return RpcResult<Res>(OI_ERR_TIMEOUT, Res{});
	}
	auto stCall = RpcCall(*stRoute.vClients[0], std::move(stRequestParam), stDeadlineParam.Remaining());
	co_return co_await stCall;
}

template <typename Res, typename Req>
coro::TaskResult<RpcResult<Res>> RpcCall(std::string sServiceParam, Req stRequestParam, ::OI::Timer::MS stTimeoutParam = ::OI::Timer::Seconds(180))
{
	return RunTargetCall<Res>(std::move(sServiceParam), std::move(stRequestParam), RpcDeadline(stTimeoutParam));
}

template <typename Res, typename Req>
auto ProtoClientManager::CallAll(const Req& stRequestParam, ::OI::Timer::MS stTimeoutParam)
{
	return CallRouted<Res>(RouteAwaiter::Kind::Named, this, sServiceName, ServicePick::All, std::string(), {}, std::string(), false, stRequestParam, stTimeoutParam);
}

template <typename Res, typename Req>
auto ProtoClientManager::CallMany(const std::vector<int32_t>& vZoneIdsParam, const Req& stRequestParam, ::OI::Timer::MS stTimeoutParam)
{
	return CallRouted<Res>(RouteAwaiter::Kind::Zones, this, sServiceName, ServicePick::Any, std::string(), vZoneIdsParam, std::string(), false, stRequestParam, stTimeoutParam);
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
