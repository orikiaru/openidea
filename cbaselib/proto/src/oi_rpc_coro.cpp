#include "oi_rpc_coro.h"
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_thread.h"
#include <atomic>
#include <cstdio>
#include <deque>
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
thread_local std::vector<std::coroutine_handle<>> g_detached_done;
}

void DetachFinished(std::coroutine_handle<> stHandleParam)
{
	if (stHandleParam)
	{
		g_detached_done.push_back(stHandleParam);
	}
}

void DrainDetached()
{
	while (!g_detached_done.empty())
	{
		std::coroutine_handle<> stHandleLocal = g_detached_done.back();
		g_detached_done.pop_back();
		if (stHandleLocal)
		{
			stHandleLocal.destroy();
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
			stHandleLocal.resume();
			DrainDetached();
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
