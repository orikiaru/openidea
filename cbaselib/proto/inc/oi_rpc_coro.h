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
void DetachFinished(std::coroutine_handle<> stHandleParam, void (*fnReleaseParam)(std::coroutine_handle<>));
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

inline SleepMs yield(std::chrono::milliseconds stDelayParam)
{
	if (stDelayParam.count() > (std::numeric_limits<int32_t>::max)())
	{
		throw std::out_of_range("yield duration too large");
	}
	return yield(stDelayParam.count() <= 0 ? 0 : static_cast<int32_t>(stDelayParam.count()));
}

// 等待节点放在调用方协程帧中，单个或多个等待者都不另分配堆节点。
struct TaskWaiter
{
	std::coroutine_handle<> stParent;
	TaskWaiter* pNext = nullptr;
	std::atomic<int> iGate{0}; // 0 登记中，1 已完成，2 已挂起。
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
			// 发布完成后等待者可能立即恢复并销毁节点，只能再用局部副本。
			auto* pNext = pWaiter->pNext;
			const auto stParent = pWaiter->stParent;
			if (pWaiter->iGate.exchange(1, std::memory_order_acq_rel) == 2)
			{
				Schedule(stParent);
			}
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
	std::atomic<size_t> iReferences{1}; // Task 对象、等待者、运行中的帧各自持有引用。
	bool bStarted = false;
	bool bReady = false;
	bool bDetached = false;
	std::suspend_always initial_suspend() noexcept
	{
		return {};
	}
	TaskFinalSuspend final_suspend() noexcept
	{
		return {};
	}
	void unhandled_exception() noexcept
	{
		stError = std::current_exception();
		if (bDetached)
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

	Token Start(std::shared_ptr<void> pCallableParam = {}, Token iTokenParam = 0) const
	{
		if (!stHandle)
		{
			throw std::logic_error("cannot start a moved-from Task");
		}
		Token iTokenLocal;
		bool bStart = false;
		{
			auto& stPromise = stHandle.promise();
			OI::Thread::MutexGuard stLock(stPromise.stMutex);
			if (stPromise.iTaskId == 0)
			{
				stPromise.iTaskId = iTokenParam ? iTokenParam : AllocTaskUid();
			}
			iTokenLocal = stPromise.iTaskId;
			if (!stPromise.bStarted)
			{
				stPromise.pCallable = std::move(pCallableParam);
				stPromise.bDetached = true;
				stPromise.bStarted = true;
				stPromise.iReferences.fetch_add(1, std::memory_order_relaxed);
				bStart = true;
			}
		}
		if (bStart)
		{
			Schedule(stHandle);
		}
		return iTokenLocal;
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
		bool await_ready() const noexcept
		{
			return false;
		}
		bool await_suspend(std::coroutine_handle<> stParentParam)
		{
			// 登记和完成共用锁；首个等待者负责启动，其他等待者只订阅。
			const auto stChildLocal = stChild;
			bool bStart = false;
			{
				auto& stPromise = stChildLocal.promise();
				OI::Thread::MutexGuard stLock(stPromise.stMutex);
				if (stPromise.bReady)
				{
					return false;
				}
				stParent = stParentParam;
				pNext = stPromise.pWaiters;
				stPromise.pWaiters = this;
				if (!stPromise.bStarted)
				{
					stPromise.bStarted = true;
					stPromise.iReferences.fetch_add(1, std::memory_order_relaxed);
					bStart = true;
				}
			}
			if (bStart)
			{
				stChildLocal.resume();
			}
			// 当场完成仍走内联返回；发布挂起后不再访问 this。
			return iGate.exchange(2, std::memory_order_acq_rel) != 1;
		}
		T await_resume()
		{
			const auto& stPromise = stChild.promise();
			if (stPromise.stError)
			{
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

template <typename F>
auto StartTask(F&& fnFunctionParam)
{
	Token iTokenLocal = AllocTaskUid();
	auto stCallable = std::make_shared<std::decay_t<F>>(std::forward<F>(fnFunctionParam));
	auto stTask = (*stCallable)(iTokenLocal);
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
		bool await_suspend(std::coroutine_handle<> stWaiterParam)
		{
			::OI::Thread::MutexGuard stLock(pState->stMutex);
			pState->bClosed = true;
			if (pState->iActive == 0)
			{
				return false;
			}
			pState->vWaiters.push_back(stWaiterParam);
			return true;
		}
		void await_resume()
		{
			::OI::Thread::MutexGuard stLock(pState->stMutex);
			if (pState->stError)
			{
				std::rethrow_exception(pState->stError);
			}
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
	~TaskScope()
	{
		RequestStop();
	}
	void RequestStop()
	{
		::OI::Thread::MutexGuard stLock(m_pState->stMutex);
		m_pState->bClosed = true;
		m_pState->pStop->store(true, std::memory_order_release);
	}
	template <typename F>
	Token Start(F&& fnFunctionParam)
	{
		::OI::Thread::MutexGuard stLock(m_pState->stMutex);
		if (m_pState->bClosed)
		{
			throw std::logic_error("TaskScope is closed");
		}
		Token iTokenLocal = AllocTaskUid();
		auto stTask = Run(m_pState, std::forward<F>(fnFunctionParam), iTokenLocal);
		stTask.stHandle.promise().iTaskId = iTokenLocal;
		++m_pState->iActive;
		return stTask.Start();
	}
	Task Join()
	{
		return Wait(m_pState);
	}
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
		auto stInvoke = [&]()
		{
			if constexpr (std::is_invocable_v<F&, Req&, Res&>)
			{
				return fnFunctionParam(stRequestLocal, stResponseLocal);
			}
			else if constexpr (std::is_invocable_v<F&, const RpcContext&, Req&, Res&>)
			{
				return fnFunctionParam(stContext, stRequestLocal, stResponseLocal);
			}
			else
			{
				return fnFunctionParam(iTokenParam, pManagerParam, iSessionIdParam, stRequestLocal, stResponseLocal); // 兼容已有五参数回调。
			}
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
			if (stChild.stHandle)
			{
				stChild.stHandle.promise().iTaskId = iTokenParam;
			}
			if constexpr (std::is_same_v<Return, coro::Task>)
			{
				co_await stChild;
			}
			else
			{
				iRetcode = co_await stChild;
			}
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
uint64_t BindRpcCall(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, proto_type_t iResponseIdParam, std::coroutine_handle<> stHandleParam, int32_t* pResultParam, int64_t iTimeoutMillisecondsParam, std::function<bool(::OI::Base::BytesStream&)> fnDecodeParam, std::function<void()> fnResumeParam);
void FailRpcCalls(::OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam);
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
	// 回包原样留到 await_resume 再解，解包跑在工人上，不占 IO 线程。
	::OI::Base::BytesStream stReply;
	bool bReplied;
	std::exception_ptr stSubmissionError;

	RpcCallAwaiter()
	    : pManager(NULL), iSessionId(0), pRequest(NULL), pResponse(NULL), iResult(OI_ERR_FAIL), iTimeoutMilliseconds(180000), stHandle(), iGate(0), bReplied(false)
	{
	}
	RpcCallAwaiter(const RpcCallAwaiter&) = delete;
	RpcCallAwaiter(RpcCallAwaiter&& stOtherParam)
	    : pManager(ClaimMove(stOtherParam)), pClient(stOtherParam.pClient), iSessionId(stOtherParam.iSessionId), pRequest(stOtherParam.pRequest), pResponse(stOtherParam.pResponse), iResult(stOtherParam.iResult),
	      iTimeoutMilliseconds(stOtherParam.iTimeoutMilliseconds), stHandle(stOtherParam.stHandle), iGate(stOtherParam.iGate.load(std::memory_order_relaxed)),
	      bReplied(false), stSubmissionError(std::move(stOtherParam.stSubmissionError))
	{
		stOtherParam.stHandle = nullptr;
	}
	static ::OI::Net::SessionManager* ClaimMove(RpcCallAwaiter& stOtherParam)
	{
		stOtherParam.ClaimAwait();
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
	template <typename Promise>
	bool Suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		stHandle = stHandleParam;
		iResult = OI_ERR_FAIL;
		bReplied = false;
		stSubmissionError = nullptr;
		iGate.store(0, std::memory_order_relaxed);
		uint64_t iCallIdLocal = BindRpcCall(pManager, iSessionId, RpcProtoId<Res>::iValue, stHandleParam, &iResult, iTimeoutMilliseconds, [this](::OI::Base::BytesStream& stInputParam)
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
		});
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
		int iPrevious = iGate.exchange(2, std::memory_order_acq_rel);
		return iPrevious != 1;
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

template <typename Req, typename Res>
RpcCallAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req& stRequestParam, Res& stResponseParam, std::chrono::milliseconds stTimeoutParam)
{
	auto stAwaiter = RpcCall(stManagerParam, iSessionIdParam, stRequestParam, stResponseParam);
	stAwaiter.iTimeoutMilliseconds = stTimeoutParam.count();
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

// 输出引用仅在成功解包后更新，错误时保留调用方原值。
template <typename Req>
auto RpcCall(ProtoClientManager& stManagerParam, Req& stRequestParam, typename RpcProtoId<Req>::Response& stResponseParam,
    std::chrono::milliseconds stTimeoutParam = ::OI::Timer::Seconds(180))
{
	auto stCall = RpcCall(stManagerParam, INVALID_SID, stRequestParam, stResponseParam, stTimeoutParam);
	stCall.pClient = &stManagerParam;
	return stCall;
}

// 拥有请求和响应，应用不必跨挂起维护输出引用。
template <typename Req, typename Res>
struct [[nodiscard]] RpcValueAwaiter : RpcCallAwaiter<Req, Res>
{
	Req stRequest;
	Res stResponse{};
	RpcValueAwaiter(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam, std::chrono::milliseconds stTimeoutParam, ProtoClientManager* pClientParam = nullptr)
	    : stRequest(std::move(stRequestParam))
	{
		this->pManager = &stManagerParam;
		this->pClient = pClientParam;
		this->iSessionId = iSessionIdParam;
		this->iTimeoutMilliseconds = stTimeoutParam.count();
	}
	template <typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> stHandleParam)
	{
		this->ClaimAwait();
		this->pRequest = &stRequest;
		this->pResponse = &stResponse;
		return RpcCallAwaiter<Req, Res>::Suspend(stHandleParam);
	}
	RpcResult<Res> await_resume()
	{
		return {RpcCallAwaiter<Req, Res>::await_resume(), std::move(stResponse)};
	}
};

template <typename Res, typename Req>
RpcValueAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam, std::chrono::milliseconds stTimeoutParam = std::chrono::seconds(180))
{
	return RpcValueAwaiter<Req, Res>(stManagerParam, iSessionIdParam, std::move(stRequestParam), stTimeoutParam);
}
// 自动配对响应类型；保留显式 Res 和输出引用接口供旧代码使用。
template <typename Req>
auto RpcCall(::OI::Net::SessionManager& stManagerParam, sid_t iSessionIdParam, Req stRequestParam,
    std::chrono::milliseconds stTimeoutParam = ::OI::Timer::Seconds(180))
    -> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {stManagerParam, iSessionIdParam, std::move(stRequestParam), stTimeoutParam};
}

template <typename Req>
auto RpcCall(ProtoClientManager& stManagerParam, Req stRequestParam,
    std::chrono::milliseconds stTimeoutParam = ::OI::Timer::Seconds(180))
    -> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {stManagerParam, INVALID_SID, std::move(stRequestParam), stTimeoutParam, &stManagerParam};
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
