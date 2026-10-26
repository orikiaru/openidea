#ifndef __j_thread_pool_h__
#define __j_thread_pool_h__
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <type_traits>
#include <utility>
#include "config.h"
#include "task.h"

typedef std::map<int, Task*> SIG_HANDLE_MAP;
class ThreadPool
{
public:
	struct ThreadData
	{
		TASK_LIST	 	_task_list;
		unsigned int 	_task_count;
		int 			_thread_count;
		std::mutex		_mutex;
		std::condition_variable _cond;
	}_thread_data[PRIO_NUM];
	
	int  _status;
	SIG_HANDLE_MAP	_sig_handle_map;
	std::thread::id	_main_thread_id;
	bool (*_drain)();
public:
	static ThreadPool* GetInstance() { static ThreadPool inst; return &inst;	}
	bool Init();
	bool AddTask(Task* pTask);
	template<typename F>
	typename std::enable_if<!std::is_base_of<Task, typename std::remove_pointer<typename std::decay<F>::type>::type>::value, bool>::type
	AddTask(F&& fn, PRIO_ENUM prio = PRIO_NORMAL)
	{
		typedef typename std::decay<F>::type Fn;
		return AddTask(new FnTask<Fn>(std::forward<F>(fn), prio));
	}
	void StartWork();
	void AddSigHandleTask(int sig, Task* pTask);
	void OnSignal(int sig);
	void SetDrain(bool (*fn)());

	static bool CreateSpecThread(std::thread::id& tid, void* (*thread_handle)(void*), void* arg);
	static void InitIgnoreSig();
	static void InitUnIgnoreSig(int sig);
private:
	void SingleThreadStartWork();
	void MultiThreadStartWork();
};



#endif
