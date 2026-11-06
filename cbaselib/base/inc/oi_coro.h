#pragma once
#include "oi_lock.h"
#include "oi_duration.h"
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

namespace OI
{
namespace Coro
{

// 协程 token。StartTask 生成并传进协程。
typedef uint64_t Token;
typedef Token TaskUid;

// 分配进程内递增的任务标识。0 留给还没分配的状态。
TaskUid AllocTaskUid();

// 挂起的协程放回运行队列。单线程进本线程队列，多线程进工人队列。RPC 回包和定时器都走这里。
void Schedule(std::coroutine_handle<> stHandle);

// 单消费者完成门。结果和挂起谁先到都能收口，不会把协程永久挂住。
// 完成方 exchange(Ready)，只有看见 Waiting 才 Schedule。
// 等待方 exchange(Waiting)，看见 Ready 就不挂起。
enum class Gate : int32_t
{
	Idle,
	Ready,
	Waiting,
};

// 根协程跑完后不能在 final_suspend 里销毁自己。登记下来，等 resume 返回再释放。
void DetachFinished(std::coroutine_handle<> stHandle, void (*fnRelease)(std::coroutine_handle<>));
// 释放已经跑完、等 resume 返回后才能销毁的根协程。
void DrainDetached();
// 调度器唯一的 resume 入口。期间新建的任务当场跑到第一次挂起，协程外新建的任务入队。
void ResumeTask(std::coroutine_handle<> stHandle);
// 当前栈是否正在 ResumeTask 里。新任务用它决定能不能当场跑。
bool InTaskResume() noexcept;

// 新任务构造 promise 时取走。只在 StartTask、TaskScope 等框架入口里设置。
struct TaskLaunch
{
	// 这条任务的标识。0 表示构造时再分配。
	Token iToken = 0;
	// 没有人等。异常由任务自己报告，不留给 co_await。
	bool bDetached = false;
	// initial_suspend 必须入队，不在调用方栈上跑。
	bool bPost = false;
};
// 取走当前线程刚设置的启动信息，取完就清空。
TaskLaunch TakeTaskLaunch() noexcept;
// 构造时压入一份启动信息，析构时恢复上一份。
class TaskLaunchScope
{
	// 进入之前线程上的那一份，析构时放回去。
	TaskLaunch m_stPrevious;

  public:
	explicit TaskLaunchScope(TaskLaunch stLaunch) noexcept;
	~TaskLaunchScope();
	TaskLaunchScope(const TaskLaunchScope&) = delete;
	TaskLaunchScope& operator=(const TaskLaunchScope&) = delete;
};
using TaskErrorHandler = void (*)(Token, std::exception_ptr);
// 在工作线程调用；处理函数必须线程安全，不允许抛异常。nullptr 恢复默认 stderr 报告。
void SetTaskErrorHandler(TaskErrorHandler fnHandler);
// 把没人接住的任务异常交给处理器。处理器不得再抛。
void ReportTaskError(Token iToken, std::exception_ptr stError) noexcept;

// 单线程就绪队列。Poll 只跑进入时已经在队列里的句柄，这一轮新挂上的留到 poll 之后。
class StScheduler
{
  public:
	// 把句柄放进本线程队列。单线程的锁是空的，不能从别的线程写。
	static void Post(std::coroutine_handle<> stHandle);
	// 只跑进入时已经在队列里的句柄。这一轮新挂上的留到下次。
	static bool Poll();
	// 队列里是否还有没跑的句柄。
	static bool Queued();
};

// 下一轮再跑，或者过 ms 毫秒再跑。到点就是 Schedule。
struct SleepMs
{
	// 再过这么久唤醒。0 表示下一轮再跑，不睡真实时间。
	int32_t iMilliseconds;
	bool await_ready() const noexcept
	{
		return false;
	}
	void await_suspend(std::coroutine_handle<> stHandle) const noexcept;
	void await_resume() const noexcept
	{
	}
};

inline SleepMs yield(int32_t iMilliseconds)
{
	return SleepMs{iMilliseconds};
}

SleepMs yield(::OI::Timer::MS stDelay);

// 等待节点放在调用方协程帧或 WhenAll 的稳定状态里，不按等待者另做包装协程。
// fnReady 为空时，完成即唤醒。WhenAll 只在最后一个任务完成时返回 true。
struct TaskWaiter
{
	// 被挂起的等待方。
	std::coroutine_handle<> stParent;
	// 同一条任务上的下一个等待者。
	TaskWaiter* pNext = nullptr;
	// 空则完成即唤醒。WhenAll 用它等到最后一个子任务。
	bool (*fnReady)(TaskWaiter* pWaiter) = nullptr;
};
// 协程里新建的任务当场跑；协程外（网络回调、main）新建的或框架要求入队的，放进运行队列。
struct TaskInitialSuspend
{
	// 真则不挂起，在当前 resume 栈上跑到第一次 co_await。
	bool bInline;
	bool await_ready() const noexcept
	{
		return bInline;
	}
	void await_suspend(std::coroutine_handle<> stHandle) const noexcept
	{
		Schedule(stHandle);
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
	void await_suspend(std::coroutine_handle<Promise> stHandle) const noexcept
	{
		auto& stPromise = stHandle.promise();
		TaskWaiter* pWaiter;
		{
			OI::Thread::MutexGuard stLock(stPromise.stMutex);
			stPromise.bReady = true;
			pWaiter = std::exchange(stPromise.pWaiters, nullptr);
		}
		while (pWaiter)
		{
			// 完成前挂上的等待者都在这张链表里。先记下下一项再 Schedule：
			// 单线程下一轮才 resume，多线程可能立刻在别的工人上 resume 并销毁节点。
			auto* pNext = pWaiter->pNext;
			const bool bWake = pWaiter->fnReady == nullptr || pWaiter->fnReady(pWaiter);
			if (bWake)
			{
				Schedule(pWaiter->stParent);
			}
			pWaiter = pNext;
		}
		// 执行引用必须等 resume 返回后才释放，不能在 final_suspend 中销毁帧。
		DetachFinished(stHandle, [](std::coroutine_handle<> stHandleLocal)
		{
			Promise::Release(std::coroutine_handle<Promise>::from_address(stHandleLocal.address()));
		});
	}
	void await_resume() const noexcept
	{
	}
};
// 任务状态。一条任务只从「在跑」进「完成」，不能再跑第二次。
//
// 在跑：bReady 为假。协程里直接调用时 bInlineStart 为真，initial_suspend 不挂起，在当前 resume 栈上跑到第一次 co_await。
//       协程外新建，或 StartTask / TaskScope::Start，bInlineStart 为假，initial_suspend 把句柄放进运行队列。
// 完成：final_suspend 在锁里把 bReady 写成真，摘走 pWaiters 再逐个 Schedule。之后的 co_await 看见 bReady，不再挂起。
// 独立：bDetached 为真表示没有人等也要自己报告异常。Start() 只打这个标记，不重新执行。
// 异常：bObserved 表示某个等待者已经重抛过；bErrorReported 保证 ReportError 只进一次。没人看的异常在最后一个引用释放时再报。
struct TaskState
{
	// 保活启动时的闭包，避免带捕获的 lambda 在协程还活着时被销毁。
	std::shared_ptr<void> pCallable;
	// 协程里没接住的异常。等待者重抛，没人看则在最后一份引用释放时报告。
	std::exception_ptr stError;
	// StartTask 或 Start 分配的标识。0 表示还没分配。
	Token iTaskId = 0;
	// 保护完成标记、等待者链表和任务标识。
	OI::Thread::Mutex stMutex;
	// 完成前挂上来的等待者。final_suspend 一次摘走。
	TaskWaiter* pWaiters = nullptr;
	std::atomic<size_t> iReferences{2}; // 创建时两份：Task 对象一份，正在跑的帧一份。每个等待器再加一份。
	bool bReady = false;                // 假=还在跑或还没开始；真=已经 co_return 或异常结束。
	bool bInlineStart = false;          // 真=initial_suspend 不挂起。
	std::atomic<bool> bDetached{false}; // 真=独立任务，异常不等待者来取。
	// 已经有等待者重抛过异常。最后一份引用据此决定还要不要再报。
	std::atomic<bool> bObserved{false};
	// ReportError 只进一次。
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
struct TaskReturn : TaskState
{
	// co_return 写下的值。按值复制给每个等待者，不从这里移走。
	std::optional<T> stResult;
	void return_value(T stValue)
	{
		stResult.emplace(std::move(stValue));
	}
};
template <>
struct TaskReturn<void> : TaskState
{
	void return_void() noexcept
	{
	}
};

// 一条任务只运行一次，可多次/并发等待；结果和异常保留到最后一个引用释放。
template <typename T = void>
struct BasicTask
{
	struct promise_type : TaskReturn<T>
	{
		BasicTask get_return_object()
		{
			return BasicTask{std::coroutine_handle<promise_type>::from_promise(*this)};
		}
		static void Release(std::coroutine_handle<promise_type> stHandle)
		{
			if (stHandle && stHandle.promise().iReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
			{
				// 没有人取走的异常在最后一个引用释放时报告，不会悄悄丢掉。
				auto& stPromise = stHandle.promise();
				if (stPromise.stError && !stPromise.bObserved.load(std::memory_order_acquire))
				{
					stPromise.ReportError();
				}
				stHandle.destroy();
			}
		}
	};
	using Handle = std::coroutine_handle<promise_type>;
	// 协程帧。空句柄表示已经移走。
	Handle stHandle;
	explicit BasicTask(Handle stHandle)
	    : stHandle(stHandle)
	{
	}
	BasicTask(BasicTask&& stOther) noexcept
	    : stHandle(std::exchange(stOther.stHandle, {}))
	{
	}
	// 外层只是共享句柄，复制不复制协程帧，也不会再次执行任务。
	BasicTask(const BasicTask& stOther) noexcept
	    : stHandle(stOther.stHandle)
	{
		if (stHandle)
		{
			stHandle.promise().iReferences.fetch_add(1, std::memory_order_relaxed);
		}
	}
	BasicTask& operator=(const BasicTask& stOther) noexcept
	{
		BasicTask stCopy(stOther);
		std::swap(stHandle, stCopy.stHandle);
		return *this;
	}
	BasicTask& operator=(BasicTask&& stOther) noexcept
	{
		if (this != &stOther)
		{
			BasicTask stMoved(std::move(stOther));
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
	Token Start(std::shared_ptr<void> pCallable = {}, Token iToken = 0) const
	{
		if (!stHandle)
		{
			throw std::logic_error("cannot start a moved-from Task");
		}
		auto& stPromise = stHandle.promise();
		OI::Thread::MutexGuard stLock(stPromise.stMutex);
		if (stPromise.iTaskId == 0)
		{
			stPromise.iTaskId = iToken ? iToken : AllocTaskUid();
		}
		if (!stPromise.pCallable)
		{
			stPromise.pCallable = std::move(pCallable);
		}
		if (!stPromise.bDetached.exchange(true, std::memory_order_acq_rel) && stPromise.bReady && stPromise.stError)
		{
			stPromise.ReportError();
		}
		return stPromise.iTaskId;
	}

	// 多个 co_await 共用这一条任务。完成前：挂进 pWaiters。完成后：await_ready 看见 bReady，不挂起。
	// 一前一后：先来的由 final_suspend 唤醒，后来的在锁里看见 bReady，当场 await_resume。
	// 结果按值复制，不从 stResult 里移走。
	struct Awaiter : TaskWaiter
	{
		Handle stChild;
		explicit Awaiter(Handle stChild)
		    : stChild(stChild)
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
		bool await_suspend(std::coroutine_handle<> stParentLocal)
		{
			// 登记和完成共用锁。登记后子任务可能在别的线程立即完成并调度本协程，之后不再访问 this。
			auto& stPromise = stChild.promise();
			OI::Thread::MutexGuard stLock(stPromise.stMutex);
			if (stPromise.bReady)
			{
				return false;
			}
			stParent = stParentLocal;
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

// 节点放在堆上。awaiter 被协程帧挪走时，已经链进任务的地址不能跟着变。
template <typename T>
struct WhenAllState
{
	struct Slot : TaskWaiter
	{
		WhenAllState* pState = nullptr;
	};
	// 调用方的任务数组。await 期间必须还活着。
	std::vector<BasicTask<T>>* pTasks = nullptr;
	// 每个子任务一个等待节点。挂上链表之后不能再重新分配。
	std::vector<Slot> vSlots;
	// 还没完成的子任务数。减到 0 才考虑唤醒父协程。
	std::atomic<size_t> iLeft{0};
	// 完成和挂起谁先到。只有从 Waiting 换成 Ready 的那一方负责唤醒。
	std::atomic<Gate> iGate{Gate::Idle};

	static bool OnReady(TaskWaiter* pWaiter)
	{
		WhenAllState* pState = static_cast<Slot*>(pWaiter)->pState;
		if (pState->iLeft.fetch_sub(1, std::memory_order_acq_rel) != 1)
		{
			return false;
		}
		const Gate iPrevious = pState->iGate.exchange(Gate::Ready, std::memory_order_acq_rel);
		return iPrevious == Gate::Waiting;
	}
};

template <typename T>
struct WhenAllAwaiter
{
	// 堆上的汇合状态。awaiter 被协程帧挪走时，已经链进任务的地址不变。
	std::shared_ptr<WhenAllState<T>> pState;

	explicit WhenAllAwaiter(std::vector<BasicTask<T>>& vTasks)
	    : pState(std::make_shared<WhenAllState<T>>())
	{
		pState->pTasks = &vTasks;
	}
	bool await_ready()
	{
		DrainDetached();
		return pState->pTasks->empty();
	}
	bool await_suspend(std::coroutine_handle<> stParent)
	{
		std::vector<BasicTask<T>>& vTasks = *pState->pTasks;
		const size_t iCount = vTasks.size();
		for (size_t iIndex = 0; iIndex < iCount; ++iIndex)
		{
			if (!vTasks[iIndex].stHandle)
			{
				throw std::logic_error("cannot await a moved-from Task");
			}
		}
		pState->vSlots.resize(iCount);
		pState->iLeft.store(iCount, std::memory_order_relaxed);
		for (size_t iIndex = 0; iIndex < iCount; ++iIndex)
		{
			typename WhenAllState<T>::Slot& stSlot = pState->vSlots[iIndex];
			stSlot.pState = pState.get();
			stSlot.stParent = stParent;
			stSlot.fnReady = &WhenAllState<T>::OnReady;
			auto& stPromise = vTasks[iIndex].stHandle.promise();
			bool bReady = false;
			{
				OI::Thread::MutexGuard stLock(stPromise.stMutex);
				if (stPromise.bReady)
				{
					bReady = true;
				}
				else
				{
					stSlot.pNext = stPromise.pWaiters;
					stPromise.pWaiters = &stSlot;
				}
			}
			if (bReady && pState->iLeft.fetch_sub(1, std::memory_order_acq_rel) == 1)
			{
				pState->iGate.exchange(Gate::Ready, std::memory_order_acq_rel);
			}
		}
		const Gate iPrevious = pState->iGate.exchange(Gate::Waiting, std::memory_order_acq_rel);
		return iPrevious != Gate::Ready;
	}
	void await_resume() const noexcept
	{
	}
};

// 等到全部结束再返回。有返回值时按原顺序给出副本。
// 某一条失败时，其余任务仍会跑完，再按序号抛出第一条异常。空列表直接完成。
inline Task WhenAll(std::vector<Task> vTasks)
{
	if (!vTasks.empty())
	{
		co_await WhenAllAwaiter<void>(vTasks);
	}
	for (auto& stTask : vTasks)
	{
		co_await stTask;
	}
}

// bool 不能走这条。vector<bool> 是特化，元素是代理引用。下面的重载改回 int32_t。
template <typename T>
    requires(!std::is_void_v<T> && !std::is_same_v<T, bool>)
[[nodiscard]] TaskResult<std::vector<T>> WhenAll(std::vector<TaskResult<T>> vTasks)
{
	static_assert(!std::is_same_v<T, bool>, "bool must not instantiate vector<bool>; WhenAll returns vector<int32_t> for TaskResult<bool>");
	if (!vTasks.empty())
	{
		co_await WhenAllAwaiter<T>(vTasks);
	}
	std::vector<T> vResults;
	vResults.reserve(vTasks.size());
	for (auto& stTask : vTasks)
	{
		vResults.push_back(co_await stTask);
	}
	co_return vResults;
}

// bool 单独 co_await 仍得到 bool。汇合时收成 0/1，避免 vector<bool> 的代理引用。
[[nodiscard]] inline TaskResult<std::vector<int32_t>> WhenAll(std::vector<TaskResult<bool>> vTasks)
{
	if (!vTasks.empty())
	{
		co_await WhenAllAwaiter<bool>(vTasks);
	}
	std::vector<int32_t> vResults;
	vResults.reserve(vTasks.size());
	for (auto& stTask : vTasks)
	{
		vResults.push_back(co_await stTask ? 1 : 0);
	}
	co_return vResults;
}

// 在 stLaunch 下调用 fnFunction，它新建的第一条任务取走这份启动信息。
template <typename F>
auto InvokeTask(TaskLaunch stLaunch, F&& fnFunction)
{
	TaskLaunchScope stScope(stLaunch);
	return std::forward<F>(fnFunction)();
}

// 闭包由任务保活，带捕获的协程 lambda 必须经这里启动。任务总是入队，不在调用方栈上跑。
template <typename F>
auto StartTask(F&& fnFunction)
{
	Token iTokenLocal = AllocTaskUid();
	auto stCallable = std::make_shared<std::decay_t<F>>(std::forward<F>(fnFunction));
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
	// 和 TaskScope 共享的停止标记。真表示不要再开工，已经挂在 IO 上的帧不会被拆掉。
	std::shared_ptr<std::atomic<bool>> m_pStop;

  public:
	explicit StopToken(std::shared_ptr<std::atomic<bool>> pStop)
	    : m_pStop(std::move(pStop))
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
		// 保护计数、等待者和第一条异常。
		::OI::Thread::Mutex stMutex;
		// 协作停止标记。RequestStop 或某个任务失败时置真。
		std::shared_ptr<std::atomic<bool>> pStop = std::make_shared<std::atomic<bool>>(false);
		// 还没跑完的任务数。减到 0 时唤醒 Join。
		size_t iActive = 0;
		// 在 Join 里挂起的协程。
		std::vector<std::coroutine_handle<>> vWaiters;
		// 已经 RequestStop 或析构，不能再 Start。
		bool bClosed = false;
		// 第一个失败任务的异常。Join 时重抛。
		std::exception_ptr stError;
	};
	// 作用域和它启动的任务共享的停止、计数和异常。
	std::shared_ptr<State> m_pState = std::make_shared<State>();
	template <typename F>
	static Task Run(std::shared_ptr<State> pState, F fnFunction, Token iToken)
	{
		try
		{
			co_await fnFunction(iToken, StopToken(pState->pStop));
		}
		catch (...)
		{
			::OI::Thread::MutexGuard stLock(pState->stMutex);
			if (!pState->stError)
			{
				pState->stError = std::current_exception();
			}
			pState->pStop->store(true, std::memory_order_release);
		}
		std::vector<std::coroutine_handle<>> vWaitersLocal;
		{
			::OI::Thread::MutexGuard stLock(pState->stMutex);
			if (--pState->iActive == 0)
			{
				vWaitersLocal.swap(pState->vWaiters);
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
		bool await_suspend(std::coroutine_handle<> stWaiter);
		void await_resume();
	};
	static Task Wait(std::shared_ptr<State> pState);

  public:
	TaskScope() = default;
	TaskScope(const TaskScope&) = delete;
	TaskScope& operator=(const TaskScope&) = delete;
	~TaskScope();
	// 置停止标记并关闭 Start。不取消已经挂在 IO 或定时器上的帧。
	void RequestStop();
	// 入队一条任务并计入还在跑的数量。作用域已关闭时抛 logic_error。
	template <typename F>
	Token Start(F&& fnFunction)
	{
		::OI::Thread::MutexGuard stLock(m_pState->stMutex);
		if (m_pState->bClosed)
		{
			throw std::logic_error("TaskScope is closed");
		}
		Token iTokenLocal = AllocTaskUid();
		// 任务入队后要等这把锁才能结束，计数晚一步加不会被提前减到零。
		auto stTask = InvokeTask(TaskLaunch{iTokenLocal, true, true}, [this, &fnFunction, iTokenLocal]()
		{
			return Run(m_pState, std::forward<F>(fnFunction), iTokenLocal);
		});
		++m_pState->iActive;
		return stTask.Start();
	}
	// 等到已启动的任务都结束。某个任务失败时重抛第一条异常。
	Task Join();
};

}
}
