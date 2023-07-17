#ifndef __j_thread_pool_h__
#define __j_thread_pool_h__
#include <pthread.h>
#include <vector>
#include "config.h"
#include "task.h"

#ifdef _SINGLE_THREAD_
#define PTHREAD_SIGMASK sigprocmask
#else
#define PTHREAD_SIGMASK pthread_sigmask
#endif

typedef std::vector<pthread_t> THREAD_LIST;
typedef std::map<int, Task*> SIG_HANDLE_MAP;
class ThreadPool
{
public:
	struct ThreadData
	{
		THREAD_LIST	 	_thread_list;
		TASK_LIST	 	_task_list;
		unsigned int 	_task_count;
		int 			_thread_count;
		pthread_mutex_t		_mutex;
		pthread_cond_t		_cond;
	}_thread_data[PRIO_NUM];
	
	int  _status;
	//主线程的signalHandle,一般是单线程不能自定制线程信号处理时,修改这个容器就行了
	SIG_HANDLE_MAP	_sig_handle_map;
	pthread_t		_main_thread_id;
public:
	static ThreadPool* GetInstance() { static ThreadPool inst; return &inst;	}
	bool Init();
	bool AddTask(Task* pTask);
	void StartWork();
	void AddSigHandleTask(int sig, Task* pTask);
	void OnSignal(int sig);

	static bool CreateSpecThread(pthread_t& pid, void* (*thread_handle)(void*), void* arg);
	static void InitIgnoreSig();
	static void InitUnIgnoreSig(int sig);
private:
	void SingleThreadStartWork();
	void MultiThreadStartWork();
};



#endif
