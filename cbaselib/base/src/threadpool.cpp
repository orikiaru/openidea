#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <csignal>
#include "base_timer_manager.h"
#include "poll.h"
#include "header.h"
#include "threadpool.h"

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
	int _sig;
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
		{
			std::unique_lock<std::mutex> lock(p_data->_mutex);
			p_data->_cond.wait(lock, [&]() { return p_data->_task_count > 0; });
			pTask = p_data->_task_list.front();
			p_data->_task_list.pop_front();
			p_data->_task_count--;
		}
		pTask->Exec();
	}
}

void sig_handle(int sig_num)
{
	ThreadPool::GetInstance()->OnSignal(sig_num);
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
		std::thread worker(thread_handle, arg);
		tid = worker.get_id();
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

void ThreadPool::SingleThreadStartWork()
{
	//屏蔽一些危险信号
	InitIgnoreSig();

	while (1)
	{
		bool ran = false;
		for (int i = PRIO_NUM - 1; i >= PRIO_NORMAL; --i)
		{
			ThreadData& thread_data = _thread_data[i];
			Task* pTask = NULL;
			while ((pTask = FetchTask(&thread_data)))
			{
				// 协程 Task 只往前走一步。co_yield 会把自己放回队尾，下一轮才轮到它。
				pTask->Exec();
				ran = true;
			}
		}
		// 队列空了才挂进 poll，等 libuv 的网络事件或定时器把循环叫醒
		if (!ran)
		{
			Poll::GetInstance()->PollWait();
		}
	}
}

void ThreadPool::MultiThreadStartWork()
{
#ifndef _SINGLE_THREAD_
	//以上是主线程需要先忽略掉的信号
	ThreadPool::InitIgnoreSig();
	for (int i = PRIO_NORMAL; i < PRIO_NUM; ++i)
	{
		ThreadData& thread_data = _thread_data[i];
		for (int n = 0; n < thread_data._thread_count; ++n)
		{
			std::thread worker(WorkerMain, &thread_data);
			worker.detach();
		}
	}

	for (;;)
	{
		std::this_thread::sleep_for(std::chrono::hours(24));
	}
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







