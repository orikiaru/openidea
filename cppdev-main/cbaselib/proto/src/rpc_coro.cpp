#include "rpc_coro.h"
#include "base_timer_manager.h"
#include "threadpool.h"
#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace
{
class CoroTask : public Task
{
	std::coroutine_handle<> _handle;
public:
	explicit CoroTask(std::coroutine_handle<> handle) : _handle(handle) {}
	void Exec()
	{
		std::coroutine_handle<> handle = _handle;
		delete this;
		handle.resume();
		if (handle.done())
		{
			handle.destroy();
		}
	}
};

void PostTask(std::coroutine_handle<> handle)
{
	if (!handle || handle.done())
	{
		return;
	}
	ThreadPool::GetInstance()->AddTask(new CoroTask(handle));
}
}

namespace coro
{
void YieldToPool::await_suspend(std::coroutine_handle<> handle) const noexcept
{
	PostTask(handle);
}

void SleepMs::await_suspend(std::coroutine_handle<> handle) const noexcept
{
	if (ms <= 0)
	{
		PostTask(handle);
		return;
	}
	std::coroutine_handle<> paused = handle;
	BaseTimerManager::GetInstance()->AddTimer([paused]() {
		PostTask(paused);
	}, ms);
}

namespace
{
std::mutex g_hub_lock;
uint64_t g_seq = 0;
uint64_t g_last = 0;
std::map<uint64_t, std::function<void(int, const void*)> > g_slots;

std::mutex g_st_lock;
std::deque<std::coroutine_handle<> > g_st_queue;

std::mutex g_mt_lock;
std::condition_variable g_mt_cv;
std::deque<std::coroutine_handle<> > g_mt_queue;
std::vector<std::thread> g_mt_threads;
bool g_mt_started = false;

void MtLoop()
{
	for (;;)
	{
		std::coroutine_handle<> handle;
		{
			std::unique_lock<std::mutex> lock(g_mt_lock);
			g_mt_cv.wait(lock, [] { return !g_mt_queue.empty(); });
			handle = g_mt_queue.front();
			g_mt_queue.pop_front();
		}
		if (handle)
		{
			handle.resume();
		}
	}
}
}

uint64_t Hub::Add(std::function<void(int, const void*)> done)
{
	std::lock_guard<std::mutex> lock(g_hub_lock);
	uint64_t id = ++g_seq;
	g_last = id;
	g_slots[id] = std::move(done);
	return id;
}

void Hub::Bind(uint64_t id, std::function<void(int, const void*)> done)
{
	std::lock_guard<std::mutex> lock(g_hub_lock);
	g_last = id;
	g_slots[id] = std::move(done);
}

uint64_t Hub::LastId()
{
	std::lock_guard<std::mutex> lock(g_hub_lock);
	return g_last;
}

int Hub::Pending()
{
	std::lock_guard<std::mutex> lock(g_hub_lock);
	return (int)g_slots.size();
}

bool Hub::CompleteRaw(uint64_t id, int code, const void* value)
{
	std::function<void(int, const void*)> done;
	{
		std::lock_guard<std::mutex> lock(g_hub_lock);
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
	PostTask(handle);
}

bool StScheduler::Poll()
{
	bool ran = false;
	for (;;)
	{
		std::coroutine_handle<> handle;
		{
			std::lock_guard<std::mutex> lock(g_st_lock);
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
			ran = true;
		}
	}
}

void MtScheduler::Start()
{
	std::lock_guard<std::mutex> lock(g_mt_lock);
	if (g_mt_started)
	{
		return;
	}
	g_mt_started = true;
	for (int i = 0; i < 2; ++i)
	{
		g_mt_threads.push_back(std::thread(MtLoop));
		g_mt_threads.back().detach();
	}
}

void MtScheduler::Post(std::coroutine_handle<> handle)
{
	PostTask(handle);
}
}
