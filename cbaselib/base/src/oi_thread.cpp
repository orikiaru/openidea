#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <csignal>
#include <future>
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_header.h"
#include "oi_thread.h"

namespace OI { namespace Thread {
static std::string GetPrioConfigString(PRIO_ENUM prio)
{
	switch (prio)
	{
	case PRIO_ENUM::PRIO_NORMAL:
		return "normal";
	case PRIO_ENUM::PRIO_URGENT:
		return "urgent";
	case PRIO_ENUM::PRIO_QUEUE:
		return "queue";
	default:
		assert(false);
		return "unknown";
	}
	assert(false);
	return "errno_prio";
}


class GeneralSignal_HandleTask : public Task
{
	int32_t _sig;
public:
	GeneralSignal_HandleTask(int sig) :Task(PRIO_ENUM::PRIO_NORMAL),_sig(sig) {}
	void Exec()
	{
		if (_sig == SIGQUIT || _sig == SIGINT)
		{
			//TODO 友好进程关闭
			exit(0);
		}
	}
};

#define DEFAULD_THREAD_COUNT 3
bool ThreadPool::Init()
{
#ifndef _SINGLE_THREAD_
	_main_thread_id = std::this_thread::get_id();
#endif
	//AddSigHandleTask(SIGINT, new GeneralSignal_HandleTask(SIGINT));
	AddSigHandleTask(SIGQUIT, new GeneralSignal_HandleTask(SIGQUIT));

#ifdef _SINGLE_THREAD_
	for(int i = PRIO_NORMAL; i < PRIO_NUM; ++i)
	{
		ThreadData& thread_data = _thread_data[i];
		thread_data._thread_count = 1;
	}
#else
	for(int i = PRIO_NORMAL; i < PRIO_NUM; ++i)
	{
		ThreadData& thread_data = _thread_data[i];

		thread_data._thread_count = DEFAULD_THREAD_COUNT;
		std::string config_key = "ThreadPool." + GetPrioConfigString((PRIO_ENUM)i);
		TryGetConfigAndReturnFalse(config_key, thread_data._thread_count);

		if (i == PRIO_ENUM::PRIO_QUEUE)
		{
			thread_data._thread_count = 1;
		}

	}
#endif
	return true;
}

Task* FetchTask(ThreadPool::ThreadData* p_data)
{
	assert(p_data);
	Task* pTask = NULL;
	if(p_data->_task_count)
	{
		pTask = p_data->_task_list.front();	
		p_data->_task_list.pop_front();
		p_data->_task_count --;
	}

	return pTask;
}

static void WorkerMain(ThreadPool::ThreadData* p_data)
{
	ThreadPool::InitIgnoreSig();
	assert(p_data);
	for (;;)
	{
		Task* pTask = NULL;
		std::coroutine_handle<> coro;
		{
			std::unique_lock<std::mutex> lock(p_data->_mutex);
			p_data->_idle++;
			p_data->_cond.wait(lock, [&]() {
				return p_data->_task_count > 0 || p_data->_coro_count > 0 || ThreadPool::GetInstance().Stopping();
			});
			p_data->_idle--;
			// 停了就走。否则 poll 上的定时器会一直塞任务，join 等不到。
			if (ThreadPool::GetInstance().Stopping())
			{
				return;
			}
			if (p_data->_coro_count > 0)
			{
				coro = p_data->_coros.front();
				p_data->_coros.pop_front();
				p_data->_coro_count--;
			}
			else
			{
				pTask = p_data->_task_list.front();
				p_data->_task_list.pop_front();
				p_data->_task_count--;
			}
		}
		if (coro)
		{
			// 只 resume。跑完的根协程由 SetAfterResume 登记的函数释放。
			coro.resume();
			if (ThreadPool::GetInstance()._after_resume)
			{
				ThreadPool::GetInstance()._after_resume();
			}
		}
		else
		{
			pTask->Exec();
		}
	}
}

void ThreadPool::PostCoroutine(std::coroutine_handle<> handle)
{
	if (!handle)
	{
		return;
	}
#ifndef _SINGLE_THREAD_
	ThreadData& thread_data = _thread_data[PRIO_NORMAL];
	bool wake = false;
	{
		std::lock_guard<std::mutex> lock(thread_data._mutex);
		thread_data._coros.push_back(handle);
		thread_data._coro_count++;
		wake = thread_data._idle > 0;
	}
	if (wake)
	{
		thread_data._cond.notify_one();
	}
#else
	(void)handle;
#endif
}

void ThreadPool::RequestStop(int code)
{
	{
		std::lock_guard<std::mutex> lock(_stop_mu);
		if (!_stop.exchange(true))
		{
			_exit_code = code;
		}
	}
	_stop_cond.notify_one();
	for (int i = PRIO_NORMAL; i < PRIO_NUM; ++i)
	{
		_thread_data[i]._cond.notify_all();
	}
}

void sig_handle(int sig_num)
{
	ThreadPool::GetInstance().OnSignal(sig_num);
}

void ThreadPool::OnSignal(int sig_num)
{
	SIG_HANDLE_MAP::iterator it = _sig_handle_map.find(sig_num);
	if (it == _sig_handle_map.end())
	{
		return;
	}
	it->second->Exec();
}

bool ThreadPool::CreateSpecThread(std::thread::id& tid, void* (*thread_handle)(void*), void* arg)
{
#ifndef _SINGLE_THREAD_
	InitIgnoreSig();
	try
	{
		// 新线程等 tid 写好再跑。线程函数里常拿自己的 id 和 tid 比。
		std::promise<void> go;
		std::shared_future<void> started = go.get_future().share();
		std::thread worker([thread_handle, arg, started]() {
			started.wait();
			thread_handle(arg);
		});
		tid = worker.get_id();
		go.set_value();
		worker.detach();
	}
	catch (...)
	{
		return false;
	}
	return true;
#else
	(void)tid;
	(void)thread_handle;
	(void)arg;
	assert(false && "ThreadPool::CreateSpecThread is singlethread");
	return false;
#endif
}

void ThreadPool::SetDrain(bool (*fn)())
{
	_drain = fn;
}

void ThreadPool::SetPending(bool (*fn)())
{
	_pending = fn;
}

void ThreadPool::SetAfterResume(void (*fn)())
{
	_after_resume = fn;
}

void ThreadPool::SingleThreadStartWork()
{
	//屏蔽一些危险信号
	InitIgnoreSig();

	while (!Stopping())
	{
		// 普通 Task 也只跑此刻队列里的。执行中再投的留到 poll 之后，避免自己把 poll 饿死。
		for (int i = PRIO_NUM - 1; i >= PRIO_NORMAL; --i)
		{
			ThreadData& thread_data = _thread_data[i];
			unsigned int n = thread_data._task_count;
			for (unsigned int k = 0; k < n; ++k)
			{
				Task* pTask = FetchTask(&thread_data);
				if (!pTask)
				{
					break;
				}
				pTask->Exec();
			}
		}
		if (_drain)
		{
			_drain();
		}
		bool pending = _pending && _pending();
		if (!pending)
		{
			for (int i = PRIO_NORMAL; i < PRIO_NUM; ++i)
			{
				if (_thread_data[i]._task_count)
				{
					pending = true;
					break;
				}
			}
		}
		// 队列里还有下一轮，就只试一次网络；谁都不跑了才堵住等。
		::OI::Net::Poll::GetInstance().PollWait(pending ? 0 : 1000);
	}
}

void ThreadPool::MultiThreadStartWork()
{
#ifndef _SINGLE_THREAD_
	//以上是主线程需要先忽略掉的信号
	ThreadPool::InitIgnoreSig();
	std::vector<std::thread> workers;
	for (int i = PRIO_NORMAL; i < PRIO_NUM; ++i)
	{
		ThreadData& thread_data = _thread_data[i];
		for (int n = 0; n < thread_data._thread_count; ++n)
		{
			workers.emplace_back(WorkerMain, &thread_data);
		}
	}

	{
		std::unique_lock<std::mutex> lock(_stop_mu);
		_stop_cond.wait(lock, [&]() { return _stop.load(); });
	}
	for (int i = PRIO_NORMAL; i < PRIO_NUM; ++i)
	{
		_thread_data[i]._cond.notify_all();
	}
	for (size_t i = 0; i < workers.size(); ++i)
	{
		workers[i].join();
	}
	// 工作线程已经退出，不会再往 poll 投任务。
	::OI::Net::Poll::GetInstance().StopAndJoin();
#endif
}

void ThreadPool::StartWork()
{
#ifdef _SINGLE_THREAD_
	SingleThreadStartWork();
#else
	MultiThreadStartWork();
#endif
}

bool ThreadPool::AddTask(Task* pTask)
{
	assert(pTask);
	int prio = pTask->_prio;
	assert(prio >= PRIO_NORMAL && prio < PRIO_NUM);
#ifdef _SINGLE_THREAD_
	ThreadData& thread_data = _thread_data[prio];	
	if(thread_data._task_count > 0xFFFFFFFF)
	{
		SAFE_DELETE(pTask);
		return false;
	}
	thread_data._task_count ++;
	thread_data._task_list.push_back(pTask);
#else
	ThreadData& thread_data = _thread_data[prio];	
	std::lock_guard<std::mutex> lock(thread_data._mutex);
	if(thread_data._task_count > 0xFFFFFFFF)
	{
		SAFE_DELETE(pTask);
		return false;
	}
	thread_data._task_count ++;
	thread_data._task_list.push_back(pTask);
	if(thread_data._task_count == 1)
	{
		thread_data._cond.notify_all();
	}
#endif
	return true;
}

void ThreadPool::AddSigHandleTask(int sig, Task* pTask)
{
	//来啥阻塞啥,回头在主线程的sig_wait调用pTask->Exec()
	assert(_sig_handle_map.find(sig) == _sig_handle_map.end());
	assert(pTask);

	_sig_handle_map[sig] = pTask;
	std::signal(sig, sig_handle);

#ifndef _SINGLE_THREAD_
	assert(_main_thread_id == std::this_thread::get_id());
#endif
}

void ThreadPool::InitIgnoreSig()
{
#ifdef SIGPIPE
	std::signal(SIGPIPE, SIG_IGN);
#endif
#ifdef SIGHUP
	std::signal(SIGHUP, SIG_IGN);
#endif
}


void ThreadPool::InitUnIgnoreSig(int sig)
{
	std::signal(sig, SIG_DFL);
}







}}
