#include "oi_coro.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_thread.h"
#include <atomic>
#include <cstdio>
#include <deque>
#include <limits>
#include <vector>
#include "oi_blocking_work_session.h"

namespace
{
void PostTask(std::coroutine_handle<> stHandle)
{
	// 刚创建或停在 co_await 上时，不能用 done() 过滤。那会把父协程丢掉，回包没人发。
	if (!stHandle)
	{
		return;
	}
#ifdef _SINGLE_THREAD_
	// 单线程只把句柄放回就绪队列，循环空了再进 poll。
	OI::Coro::StScheduler::Post(stHandle);
#else
	// 多线程把句柄丢给工人。协程一挂起，resume 就返回，工人接着拿下一个。
	::OI::Thread::ThreadPool::GetInstance().PostCoroutine(stHandle);
#endif
}

std::atomic<uint64_t> g_task_uid(1);

// 纯协程程序也必须接入主循环和帧回收；不能依赖链接器恰好拉入 RPC/HTTP 的目标文件。
struct SchedulerRegistration
{
	SchedulerRegistration()
	{
		auto& stPool = OI::Thread::ThreadPool::GetInstance();
		stPool.SetDrain(OI::Coro::StScheduler::Poll);
		stPool.SetPending(OI::Coro::StScheduler::Queued);
		stPool.SetResume(OI::Coro::ResumeTask);
	}
} g_stSchedulerRegistration;
}

namespace OI
{
namespace Coro
{
namespace
{
std::atomic<TaskErrorHandler> g_error_handler{nullptr};
}
void SetTaskErrorHandler(TaskErrorHandler fnHandler)
{
	g_error_handler.store(fnHandler);
}
void ReportTaskError(Token iToken, std::exception_ptr stError) noexcept
{
	if (auto fnHandler = g_error_handler.load())
	{
		try
		{
			fnHandler(iToken, stError);
			return;
		}
		catch (...)
		{
			fprintf(stderr, "coroutine error handler threw\n");
		}
	}
	try
	{
		std::rethrow_exception(stError);
	}
	catch (const std::exception& stErrorLocal)
	{
		fprintf(stderr, "coroutine %llu failed: %s\n", (unsigned long long)iToken, stErrorLocal.what());
	}
	catch (...)
	{
		fprintf(stderr, "coroutine %llu failed\n", (unsigned long long)iToken);
	}
}

TaskUid AllocTaskUid()
{
	TaskUid iTaskIdLocal = g_task_uid.fetch_add(1, std::memory_order_relaxed);
	if (iTaskIdLocal == 0)
	{
		iTaskIdLocal = g_task_uid.fetch_add(1, std::memory_order_relaxed);
	}
	return iTaskIdLocal;
}

namespace
{
struct FinishedTask
{
	std::coroutine_handle<> stHandle;
	void (*fnRelease)(std::coroutine_handle<>);
};
thread_local std::vector<FinishedTask> g_detached_done;
thread_local int32_t g_iResumeDepth = 0;
thread_local TaskLaunch g_stLaunch;
}

void ResumeTask(std::coroutine_handle<> stHandle)
{
	// 深度大于 0 时，新建且未要求入队的任务当场跑。深度在 resume 返回后才减，嵌套调用也算在协程里。
	++g_iResumeDepth;
	stHandle.resume();
	--g_iResumeDepth;
	DrainDetached();
}

bool InTaskResume() noexcept
{
	return g_iResumeDepth > 0;
}

void CheckTaskStartThread()
{
#ifdef _SINGLE_THREAD_
	if (!OI::Net::Poll::GetInstance().InLoopThread())
	{
		// 不走异步日志客户端：违规线程本来就不能访问单线程的会话发送队列。
		fprintf(stderr, "warning: coroutine start rejected: current thread is not the single-thread poll thread\n");
		fflush(stderr);
		throw std::logic_error("single-thread coroutine must start on the poll thread");
	}
#endif
}

TaskLaunch TakeTaskLaunch() noexcept
{
	return std::exchange(g_stLaunch, TaskLaunch{});
}

TaskLaunchScope::TaskLaunchScope(TaskLaunch stLaunch) noexcept
    : m_stPrevious(std::exchange(g_stLaunch, stLaunch))
{
}

TaskLaunchScope::~TaskLaunchScope()
{
	g_stLaunch = m_stPrevious;
}

void DetachFinished(std::coroutine_handle<> stHandle, void (*fnRelease)(std::coroutine_handle<>))
{
	if (stHandle)
	{
		g_detached_done.push_back({stHandle, fnRelease});
	}
}

void DrainDetached()
{
	while (!g_detached_done.empty())
	{
		const auto stFinished = g_detached_done.back();
		g_detached_done.pop_back();
		if (stFinished.stHandle)
		{
			stFinished.fnRelease(stFinished.stHandle);
		}
	}
}

void Schedule(std::coroutine_handle<> stHandle)
{
	PostTask(stHandle);
}

namespace Detail
{
void SubmitBlocking(std::shared_ptr<BlockingOperation> pOperation)
{
	OI::Net::BlockingWorkSessionManager::GetInstance().Submit(pOperation->iType, [pOperation]()
	{
		pOperation->Execute();
	}, [pOperation](int32_t iStatus, std::exception_ptr stError)
	{
		pOperation->iStatus = iStatus;
		pOperation->stError = stError;
		const auto stWaiter = pOperation->stWaiter;
		if (pOperation->iGate.exchange(Gate::Ready, std::memory_order_acq_rel) == Gate::Waiting)
		{
			Schedule(stWaiter);
		}
	});
}
}

struct WakeLater
{
	std::coroutine_handle<> stHandle;
	int32_t iMilliseconds;
};

static void ArmWakeLater(void* pArgument)
{
	WakeLater* pLater = (WakeLater*)pArgument;
	std::coroutine_handle<> stHandleLocal = pLater->stHandle;
	int32_t iMillisecondsLocal = pLater->iMilliseconds;
	delete pLater;
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([stHandleLocal]()
	{
		PostTask(stHandleLocal);
	}, iMillisecondsLocal);
}

void SleepMs::await_suspend(std::coroutine_handle<> stHandle) const noexcept
{
	// 时间轮只能在 poll 线程上改。到点再把句柄放回队列，不在定时器回调里 resume。
	if (iMilliseconds <= 0)
	{
		PostTask(stHandle);
		return;
	}
	WakeLater* pLater = new WakeLater;
	pLater->stHandle = stHandle;
	pLater->iMilliseconds = iMilliseconds;
	::OI::Net::Poll::GetInstance().CallOnLoop(ArmWakeLater, pLater);
}

namespace
{
::OI::Thread::Mutex g_st_lock;
std::deque<std::coroutine_handle<>> g_st_queue;
}

void StScheduler::Post(std::coroutine_handle<> stHandle)
{
#ifdef _SINGLE_THREAD_
	if (!stHandle)
	{
		return;
	}
	::OI::Thread::MutexGuard stLock(g_st_lock);
	g_st_queue.push_back(stHandle);
#else
	PostTask(stHandle);
#endif
}

bool StScheduler::Poll()
{
	// 把进来时的队列整批拿走。resume 里 yield(0)、StartTask 进的是原队列，这次循环碰不到。
	std::deque<std::coroutine_handle<>> vBatch;
	{
		::OI::Thread::MutexGuard stLock(g_st_lock);
		vBatch.swap(g_st_queue);
	}
	bool bRan = false;
	for (size_t iIndexLocal = 0; iIndexLocal < vBatch.size(); ++iIndexLocal)
	{
		std::coroutine_handle<> stHandleLocal = vBatch[iIndexLocal];
		if (stHandleLocal && !stHandleLocal.done())
		{
			ResumeTask(stHandleLocal);
			bRan = true;
		}
	}
	return bRan;
}

bool StScheduler::Queued()
{
	::OI::Thread::MutexGuard stLock(g_st_lock);
	return !g_st_queue.empty();
}
}
}

namespace OI
{
namespace Coro
{
SleepMs yield(::OI::Timer::MS stDelay)
{
	if (stDelay.iMilliseconds > (std::numeric_limits<int32_t>::max)())
	{
		throw std::out_of_range("yield duration too large");
	}
	return yield(stDelay.iMilliseconds <= 0 ? 0 : static_cast<int32_t>(stDelay.iMilliseconds));
}

bool TaskScope::JoinAwaiter::await_suspend(std::coroutine_handle<> stWaiter)
{
	::OI::Thread::MutexGuard stLock(pState->stMutex);
	pState->bClosed = true;
	if (pState->iActive == 0)
	{
		return false;
	}
	pState->vWaiters.push_back(stWaiter);
	return true;
}

void TaskScope::JoinAwaiter::await_resume()
{
	::OI::Thread::MutexGuard stLock(pState->stMutex);
	if (pState->stError)
	{
		std::rethrow_exception(pState->stError);
	}
}

Task TaskScope::Wait(std::shared_ptr<State> pState)
{
	co_await JoinAwaiter{std::move(pState)};
}

TaskScope::~TaskScope()
{
	RequestStop();
}

void TaskScope::RequestStop()
{
	::OI::Thread::MutexGuard stLock(m_pState->stMutex);
	m_pState->bClosed = true;
	m_pState->pStop->store(true, std::memory_order_release);
}

Task TaskScope::Join()
{
	return Wait(m_pState);
}
}
}
