#include "rpc_coro.h"
#include "base_timer_manager.h"
#include "poll.h"
#include "threadpool.h"
#include <atomic>
#include <cstdio>
#include <deque>
#include <vector>

namespace
{
void PostTask(std::coroutine_handle<> handle)
{
	// 刚创建或停在 co_await 上时，不能用 done() 过滤。那会把父协程丢掉，回包没人发。
	if (!handle)
	{
		return;
	}
#ifdef _SINGLE_THREAD_
	// 单线程只把句柄放回就绪队列，循环空了再进 poll。
	coro::StScheduler::Post(handle);
#else
	// 多线程把句柄丢给工人。协程一挂起，resume 就返回，工人接着拿下一个。
	OI::Thread::ThreadPool::GetInstance().PostCoroutine(handle);
#endif
}

std::atomic<uint64_t> g_task_uid(1);
}

namespace coro
{
TaskUid AllocTaskUid()
{
	TaskUid uid = g_task_uid.fetch_add(1, std::memory_order_relaxed);
	if (uid == 0)
	{
		uid = g_task_uid.fetch_add(1, std::memory_order_relaxed);
	}
	return uid;
}

namespace
{
thread_local std::vector<std::coroutine_handle<> > g_detached_done;
}

void DetachFinished(std::coroutine_handle<> handle)
{
	if (handle)
	{
		g_detached_done.push_back(handle);
	}
}

void DrainDetached()
{
	while (!g_detached_done.empty())
	{
		std::coroutine_handle<> handle = g_detached_done.back();
		g_detached_done.pop_back();
		if (handle)
		{
			handle.destroy();
		}
	}
}

void Schedule(std::coroutine_handle<> handle)
{
	PostTask(handle);
}

struct WakeLater
{
	std::coroutine_handle<> handle;
	int ms;
};

static void ArmWakeLater(void* arg)
{
	WakeLater* later = (WakeLater*)arg;
	std::coroutine_handle<> handle = later->handle;
	int ms = later->ms;
	delete later;
	BaseTimerManager::GetInstance().AddTimer([handle]() {
		PostTask(handle);
	}, ms);
}

void SleepMs::await_suspend(std::coroutine_handle<> handle) const noexcept
{
	// 时间轮只能在 poll 线程上改。到点再把句柄放回队列，不在定时器回调里 resume。
	if (ms <= 0)
	{
		PostTask(handle);
		return;
	}
	WakeLater* later = new WakeLater;
	later->handle = handle;
	later->ms = ms;
	Poll::GetInstance().CallOnLoop(ArmWakeLater, later);
}

namespace
{
OI::Thread::Mutex g_st_lock;
std::deque<std::coroutine_handle<> > g_st_queue;
}

void StScheduler::Post(std::coroutine_handle<> handle)
{
#ifdef _SINGLE_THREAD_
	if (!handle)
	{
		return;
	}
	OI::Thread::MutexGuard lock(g_st_lock);
	g_st_queue.push_back(handle);
#else
	PostTask(handle);
#endif
}

bool StScheduler::Poll()
{
	// 把进来时的队列整批拿走。resume 里 yield(0)、StartTask 进的是原队列，这次循环碰不到。
	std::deque<std::coroutine_handle<> > batch;
	{
		OI::Thread::MutexGuard lock(g_st_lock);
		batch.swap(g_st_queue);
	}
	bool ran = false;
	for (size_t i = 0; i < batch.size(); ++i)
	{
		std::coroutine_handle<> handle = batch[i];
		if (handle && !handle.done())
		{
			handle.resume();
			DrainDetached();
			ran = true;
		}
	}
	return ran;
}

bool StScheduler::Queued()
{
	OI::Thread::MutexGuard lock(g_st_lock);
	return !g_st_queue.empty();
}
}
