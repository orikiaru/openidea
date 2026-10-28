#ifndef __j_thread_pool_h__
#define __j_thread_pool_h__
#include <atomic>
#include <coroutine>
#include <deque>
#include <map>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <vector>
#include <type_traits>
#include <utility>
#include "oi_config.h"
#include "oi_singleton.h"
#include "oi_task.h"
#include <cstdint>

namespace OI { namespace Thread {
typedef std::map<int32_t, Task*> SIG_HANDLE_MAP;
class ThreadPool
{
	DECLARE_SINGLETON(ThreadPool)
	ThreadPool() = default;
public:
	struct ThreadData
	{
		TASK_LIST	 	_task_list;
		uint32_t	 	_task_count;
		// 协程句柄运行队列。工人取到就 resume，挂起后线程立刻去拿下一个。
		std::deque<std::coroutine_handle<> > _coros;
		uint32_t	 	_coro_count = 0;
		// 正睡在 _cond 上的工人数。为 0 时投递不用 notify，省一次 futex。
		int32_t			_idle = 0;
		int32_t			_thread_count;
		std::mutex		_mutex;
		std::condition_variable _cond;
	}_thread_data[PRIO_NUM];
	
	int32_t _status;
	SIG_HANDLE_MAP	_sig_handle_map;
	std::thread::id	_main_thread_id;
	bool (*_drain)() = nullptr;
	bool (*_pending)() = nullptr;
	void (*_after_resume)() = nullptr;
	std::atomic<bool> _stop{false};
	int32_t _exit_code = 0;
	std::mutex _stop_mu;
	std::condition_variable _stop_cond;
public:
	bool Init();
	// 多线程把协程句柄放进运行队列并叫醒一个工人。不分配 CoroTask。
	void PostCoroutine(std::coroutine_handle<> handle);
	template<typename F>
	typename std::enable_if<!std::is_base_of<Task, typename std::remove_pointer<typename std::decay<F>::type>::type>::value, bool>::type
	AddTask(F&& fn, PRIO_ENUM prio = PRIO_NORMAL)
	{
		typedef typename std::decay<F>::type Fn;
		return AddTask(new FnTask<Fn>(std::forward<F>(fn), prio));
	}
	void StartWork();
	// 让工作线程把手头的任务做完后退出。主线程在 StartWork 里 join，再回到 main 析构。
	void RequestStop(int code);
	bool Stopping() const { return _stop.load(); }
	int32_t ExitCode() const { return _exit_code; }
	void OnSignal(int sig);
	void SetDrain(bool (*fn)());
	// 单线程这一轮没跑到、但已经挂进协程队列的。yield(0) 靠它留到下一轮。
	void SetPending(bool (*fn)());
	// 协程 resume 返回后调用。用来释放已经跑完的根协程，不在 base 里直接依赖 proto。
	void SetAfterResume(void (*fn)());

	static bool CreateSpecThread(std::thread::id& tid, void* (*thread_handle)(void*), void* arg);
	static void InitIgnoreSig();
	static void InitUnIgnoreSig(int sig);
private:
	bool AddTask(Task* pTask);
	void AddSigHandleTask(int sig, Task* pTask);
	void SingleThreadStartWork();
	void MultiThreadStartWork();
};
}}

#endif
