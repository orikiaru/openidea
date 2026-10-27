#include "rpc_coro.h"
#include "base_timer_manager.h"
#include "poll.h"
#include "threadpool.h"
#include <atomic>
#include <cstdio>
#include <sstream>
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
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
	ThreadPool::GetInstance()->PostCoroutine(handle);
#endif
}

std::atomic<uint64_t> g_task_uid(1);
Mutex g_park_mu;
std::map<uint64_t, std::coroutine_handle<> > g_parked;
std::map<uint64_t, bool> g_wake_pending;
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

void NotePollWake()
{
	std::ostringstream os;
	os << std::this_thread::get_id();
	printf("rpc poll wake tid=%s in_poll=%d\n", os.str().c_str(), Poll::GetInstance()->InLoopThread() ? 1 : 0);
	fflush(stdout);
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

bool Wake(TaskUid uid)
{
	std::coroutine_handle<> handle;
	{
		MutexGuard lock(g_park_mu);
		std::map<uint64_t, std::coroutine_handle<> >::iterator it = g_parked.find(uid);
		if (it == g_parked.end())
		{
			g_wake_pending[uid] = true;
			return false;
		}
		handle = it->second;
		g_parked.erase(it);
	}
	PostTask(handle);
	return true;
}

bool ParkAt(TaskUid uid, std::coroutine_handle<> handle)
{
	MutexGuard lock(g_park_mu);
	std::map<uint64_t, bool>::iterator pending = g_wake_pending.find(uid);
	if (pending != g_wake_pending.end())
	{
		g_wake_pending.erase(pending);
		return false;
	}
	g_parked[uid] = handle;
	return true;
}

void YieldToPool::await_suspend(std::coroutine_handle<> handle) const noexcept
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
	BaseTimerManager::GetInstance()->AddTimer([handle]() {
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
	Poll::GetInstance()->CallOnLoop(ArmWakeLater, later);
}

namespace
{
Mutex g_hub_lock;
uint64_t g_seq = 0;
uint64_t g_last = 0;
std::map<uint64_t, std::function<void(int, const void*)> > g_slots;

Mutex g_st_lock;
std::deque<std::coroutine_handle<> > g_st_queue;
}

uint64_t Hub::Add(std::function<void(int, const void*)> done)
{
	MutexGuard lock(g_hub_lock);
	uint64_t id = ++g_seq;
	g_last = id;
	g_slots[id] = std::move(done);
	return id;
}

void Hub::Bind(uint64_t id, std::function<void(int, const void*)> done)
{
	MutexGuard lock(g_hub_lock);
	g_last = id;
	g_slots[id] = std::move(done);
}

uint64_t Hub::LastId()
{
	MutexGuard lock(g_hub_lock);
	return g_last;
}

int Hub::Pending()
{
	MutexGuard lock(g_hub_lock);
	return (int)g_slots.size();
}

bool Hub::CompleteRaw(uint64_t id, int code, const void* value)
{
	std::function<void(int, const void*)> done;
	{
		MutexGuard lock(g_hub_lock);
		std::map<uint64_t, std::function<void(int, const void*)> >::iterator it = g_slots.find(id);
		if (it == g_slots.end())
		{
			return false;
		}
		done = std::move(it->second);
		g_slots.erase(it);
	}
	done(code, value);
	return true;
}

void StScheduler::Post(std::coroutine_handle<> handle)
{
#ifdef _SINGLE_THREAD_
	if (!handle)
	{
		return;
	}
	MutexGuard lock(g_st_lock);
	g_st_queue.push_back(handle);
#else
	PostTask(handle);
#endif
}

bool StScheduler::Poll()
{
	// 单线程主循环在普通任务跑完后调用。这里把就绪协程跑完，一个都没有再回去进 poll。
	bool ran = false;
	for (;;)
	{
		std::coroutine_handle<> handle;
		{
			MutexGuard lock(g_st_lock);
			if (g_st_queue.empty())
			{
				return ran;
			}
			handle = g_st_queue.front();
			g_st_queue.pop_front();
		}
		if (handle && !handle.done())
		{
			handle.resume();
			DrainDetached();
			ran = true;
		}
	}
}
}
