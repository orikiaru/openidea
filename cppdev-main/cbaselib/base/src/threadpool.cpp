#include <assert.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/time.h>
#include <signal.h>
#include <stdlib.h>
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
	_main_thread_id = pthread_self();
#endif
	//AddSigHandleTask(SIGINT, new GeneralSignal_HandleTask(SIGINT));
	AddSigHandleTask(SIGQUIT, new GeneralSignal_HandleTask(SIGQUIT));

#ifdef _SINGLE_THREAD_
	for(int i = PRIO_NORMAL; i < PRIO_NUM; ++i)
	{
		ThreadData& thread_data = _thread_data[i];
		thread_data._thread_count = 1;
		thread_data._thread_list.resize(thread_data._thread_count);
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

		thread_data._thread_list.resize(thread_data._thread_count);

		pthread_mutex_init(&thread_data._mutex, NULL);
		pthread_cond_init(&thread_data._cond, NULL);
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

void *_g_start_work(void *arg)
{	
	//工作线程不处理任何相关进程逻辑信号
	ThreadPool::InitIgnoreSig();

	ThreadPool::ThreadData* p_data = (ThreadPool::ThreadData* )arg;
	//默认不做线程分离,以为不是动态的线程池,没有意义,并不需要托管回收线程资源
	//pthread_detach(pthread_self());
	assert(p_data);
	while(1)
	{
		Task* pTask = NULL;
		pthread_mutex_lock(&p_data->_mutex);
		if(p_data->_task_count)
		{
			pTask = p_data->_task_list.front();	
			p_data->_task_list.pop_front();
			p_data->_task_count --;
			pthread_mutex_unlock(&p_data->_mutex);
			pTask->Exec();
		}
		else
		{
			//no task wait the pthread_cond_broadcast after ThreadPool::AddTask
			pthread_cond_wait(&p_data->_cond, &p_data->_mutex);
			pthread_mutex_unlock(&p_data->_mutex);
		}
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

bool ThreadPool::CreateSpecThread(pthread_t& pid, void* (*thread_handle)(void*), void* arg)
{
#ifndef _SINGLE_THREAD_
	InitIgnoreSig();
	int ret = pthread_create(&pid, NULL, thread_handle, arg);
	if (ret != 0)
	{
		perror("pthread_create");
		return false;
	}
	return true;
#else
	assert(false && "ThreadPool::CreateSpecThread is singlethread");
#endif
}

class PollTask : public Task
{
public:
	PollTask() :Task(PRIO_NORMAL) {}
	virtual void Exec()
	{
		Poll::GetInstance()->PollWait();
		ThreadPool::GetInstance()->AddTask(this);
	}
};

void ThreadPool::SingleThreadStartWork()
{
	//屏蔽一些危险信号
	InitIgnoreSig();

	//单线程直接执行信号函数
	sigset_t sig_set;
	sigemptyset(&sig_set);
	for (SIG_HANDLE_MAP::iterator it = _sig_handle_map.begin(); it != _sig_handle_map.end(); ++it)
	{
		sigaddset(&sig_set, it->first);
		signal(it->first, sig_handle);
	}
	PTHREAD_SIGMASK(SIG_UNBLOCK, &sig_set, 0);

	//添加基础pollio任务,单线程一切任务由线程池驱动,定时器则可以随时打断pollio
	Task* pTask = new PollTask;
	AddTask(pTask);

	while (1)
	{
		//不是在pollio,就是在执行任务或者定时器
		for (int i = PRIO_NUM; i > PRIO_NORMAL; --i)
		{
			ThreadData& thread_data = _thread_data[i - 1];
			while ((pTask = FetchTask(&thread_data)))
			{
				pTask->Exec();
			}
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
		for (int i = 0; i < thread_data._thread_count; ++i)
		{
			if (pthread_create(&thread_data._thread_list[i], NULL, _g_start_work, &thread_data) == -1)
			{
				perror("pthread_create");
			}
		}
	}
	sigset_t sig_set;
	sigemptyset(&sig_set);
	for (SIG_HANDLE_MAP::iterator it = _sig_handle_map.begin(); it != _sig_handle_map.end(); ++it)
	{
		sigaddset(&sig_set, it->first);
	}

	//主线程在这等待信号处理即可
	while (1)
	{
		int sig_id = -1;
		if (0 == sigwait(&sig_set, &sig_id))
		{
			//TODO 做一些热更新?
			sig_handle(sig_id);
		}
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
	pthread_mutex_lock(&thread_data._mutex);
	if(thread_data._task_count > 0xFFFFFFFF)
	{
		SAFE_DELETE(pTask);
		pthread_mutex_unlock(&thread_data._mutex);
		return false;
	}
	thread_data._task_count ++;
	thread_data._task_list.push_back(pTask);
	if(thread_data._task_count == 1)	//old no task try awark all thread to work
	{
		pthread_cond_broadcast(&thread_data._cond);
	}
	pthread_mutex_unlock(&thread_data._mutex);
#endif
	return true;
}

void ThreadPool::AddSigHandleTask(int sig, Task* pTask)
{
	//来啥阻塞啥,回头在主线程的sig_wait调用pTask->Exec()
	assert(_sig_handle_map.find(sig) == _sig_handle_map.end());
	assert(pTask);

	_sig_handle_map[sig] = pTask;

	sigset_t sig_set;
	sigemptyset(&sig_set);
	sigaddset(&sig_set, sig);
	PTHREAD_SIGMASK(SIG_BLOCK, &sig_set, 0);

#ifndef _SINGLE_THREAD_
	assert(_main_thread_id == pthread_self());
#endif
}

void ThreadPool::InitIgnoreSig()
{
	//当前线程屏蔽信号,只屏蔽一些常用的可能会自定义的信号处理,其他使用缺省处理
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, SIGPIPE);
	sigaddset(&set, SIGHUP);

	if (0 != PTHREAD_SIGMASK(SIG_BLOCK, &set, 0))
	{
		perror("ThreadPool::InitIgnoreSig pthread_sigmask");
	}
}


void ThreadPool::InitUnIgnoreSig(int sig)
{
	sigset_t set;
	sigemptyset(&set);
	sigaddset(&set, sig);
	if (0 != PTHREAD_SIGMASK(SIG_UNBLOCK, &set, 0))
	{
		perror("ThreadPool::InitUnIgnoreSig pthread_sigmask");
	}
}







