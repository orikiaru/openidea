#include "oi_rpc_coro.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_thread.h"
#include <atomic>
#include <cstdio>
#include <deque>
#include <limits>
#include <vector>

namespace
{
void PostTask(std::coroutine_handle<> stHandleParam)
{
	// 刚创建或停在 co_await 上时，不能用 done() 过滤。那会把父协程丢掉，回包没人发。
	if (!stHandleParam)
	{
		return;
	}
#ifdef _SINGLE_THREAD_
	// 单线程只把句柄放回就绪队列，循环空了再进 poll。
	coro::StScheduler::Post(stHandleParam);
#else
	// 多线程把句柄丢给工人。协程一挂起，resume 就返回，工人接着拿下一个。
	::OI::Thread::ThreadPool::GetInstance().PostCoroutine(stHandleParam);
#endif
}

std::atomic<uint64_t> g_task_uid(1);

// 纯协程程序也必须接入主循环和帧回收；不能依赖链接器恰好拉入 RPC/HTTP 的目标文件。
struct SchedulerRegistration
{
	SchedulerRegistration()
	{
		auto& stPool = OI::Thread::ThreadPool::GetInstance();
		stPool.SetDrain(coro::StScheduler::Poll);
		stPool.SetPending(coro::StScheduler::Queued);
		stPool.SetResume(coro::ResumeTask);
	}
} g_stSchedulerRegistration;
}

namespace coro
{
namespace
{
std::atomic<TaskErrorHandler> g_error_handler{nullptr};
}
void SetTaskErrorHandler(TaskErrorHandler fnHandlerParam)
{
	g_error_handler.store(fnHandlerParam);
}
void ReportTaskError(Token iTokenParam, std::exception_ptr stErrorParam) noexcept
{
	if (auto fnHandler = g_error_handler.load())
	{
		try
		{
			fnHandler(iTokenParam, stErrorParam);
			return;
		}
		catch (...)
		{
			fprintf(stderr, "coroutine error handler threw\n");
		}
	}
	try
	{
		std::rethrow_exception(stErrorParam);
	}
	catch (const std::exception& stErrorLocal)
	{
		fprintf(stderr, "coroutine %llu failed: %s\n", (unsigned long long)iTokenParam, stErrorLocal.what());
	}
	catch (...)
	{
		fprintf(stderr, "coroutine %llu failed\n", (unsigned long long)iTokenParam);
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

void ResumeTask(std::coroutine_handle<> stHandleParam)
{
	// 深度大于 0 时，新建且未要求入队的任务当场跑。深度在 resume 返回后才减，嵌套调用也算在协程里。
	++g_iResumeDepth;
	stHandleParam.resume();
	--g_iResumeDepth;
	DrainDetached();
}

bool InTaskResume() noexcept
{
	return g_iResumeDepth > 0;
}

TaskLaunch TakeTaskLaunch() noexcept
{
	return std::exchange(g_stLaunch, TaskLaunch{});
}

TaskLaunchScope::TaskLaunchScope(TaskLaunch stLaunchParam) noexcept
    : m_stPrevious(std::exchange(g_stLaunch, stLaunchParam))
{
}

TaskLaunchScope::~TaskLaunchScope()
{
	g_stLaunch = m_stPrevious;
}

void DetachFinished(std::coroutine_handle<> stHandleParam, void (*fnReleaseParam)(std::coroutine_handle<>))
{
	if (stHandleParam)
	{
		g_detached_done.push_back({stHandleParam, fnReleaseParam});
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

void Schedule(std::coroutine_handle<> stHandleParam)
{
	PostTask(stHandleParam);
}

struct WakeLater
{
	std::coroutine_handle<> stHandle;
	int32_t iMilliseconds;
};

static void ArmWakeLater(void* pArgumentParam)
{
	WakeLater* pLater = (WakeLater*)pArgumentParam;
	std::coroutine_handle<> stHandleLocal = pLater->stHandle;
	int32_t iMillisecondsLocal = pLater->iMilliseconds;
	delete pLater;
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([stHandleLocal]()
	{
		PostTask(stHandleLocal);
	}, iMillisecondsLocal);
}

void SleepMs::await_suspend(std::coroutine_handle<> stHandleParam) const noexcept
{
	// 时间轮只能在 poll 线程上改。到点再把句柄放回队列，不在定时器回调里 resume。
	if (iMilliseconds <= 0)
	{
		PostTask(stHandleParam);
		return;
	}
	WakeLater* pLater = new WakeLater;
	pLater->stHandle = stHandleParam;
	pLater->iMilliseconds = iMilliseconds;
	::OI::Net::Poll::GetInstance().CallOnLoop(ArmWakeLater, pLater);
}

namespace
{
::OI::Thread::Mutex g_st_lock;
std::deque<std::coroutine_handle<>> g_st_queue;
}

void StScheduler::Post(std::coroutine_handle<> stHandleParam)
{
#ifdef _SINGLE_THREAD_
	if (!stHandleParam)
	{
		return;
	}
	::OI::Thread::MutexGuard stLock(g_st_lock);
	g_st_queue.push_back(stHandleParam);
#else
	PostTask(stHandleParam);
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

namespace coro
{
SleepMs yield(::OI::Timer::MS stDelayParam)
{
	if (stDelayParam.iMilliseconds > (std::numeric_limits<int32_t>::max)())
	{
		throw std::out_of_range("yield duration too large");
	}
	return yield(stDelayParam.iMilliseconds <= 0 ? 0 : static_cast<int32_t>(stDelayParam.iMilliseconds));
}

bool TaskScope::JoinAwaiter::await_suspend(std::coroutine_handle<> stWaiterParam)
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

void TaskScope::JoinAwaiter::await_resume()
{
	::OI::Thread::MutexGuard stLock(pState->stMutex);
	if (pState->stError)
	{
		std::rethrow_exception(pState->stError);
	}
}

Task TaskScope::Wait(std::shared_ptr<State> pStateParam)
{
	co_await JoinAwaiter{std::move(pStateParam)};
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
