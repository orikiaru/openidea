#pragma once
#include <atomic>
#include <exception>
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
// 普通任务异常在调度入口隔离；处理器不得抛异常，nullptr 恢复 stderr 报告。
using TaskErrorHandler = void (*)(std::exception_ptr);
void SetTaskErrorHandler(TaskErrorHandler fnHandlerParam);
void ReportTaskError(std::exception_ptr stErrorParam) noexcept;

class ThreadPool
{
	DECLARE_SINGLETON(ThreadPool)
	ThreadPool() = default;
public:
	struct ThreadData
	{
		TASK_LIST	 	vTaskList;
		uint32_t	 	iTaskCount;
		// 协程句柄运行队列。工人取到就 resume，挂起后线程立刻去拿下一个。
		std::deque<std::coroutine_handle<> > vCoroutines;
		uint32_t	 	iCoroutineCount = 0;
		// 正睡在 _cond 上的工人数。为 0 时投递不用 notify，省一次 futex。
		int32_t			iIdleThreadCount = 0;
		int32_t			iThreadCount;
		std::mutex		stMutex;
		std::condition_variable stCondition;
	}vThreadData[PRIO_NUM];
	
	int32_t iStatus;
	// 每个优先级队列里最多攒多少个 Task。配置 [ThreadPool] max_task_count，不配不限。
	uint32_t iMaxTaskCount = UINT32_MAX;
	bool (*fnDrain)() = nullptr;
	bool (*fnPending)() = nullptr;
	void (*fnAfterResume)() = nullptr;
	std::atomic<bool> bStop{false};
	int32_t iExitCode = 0;
	std::mutex stStopMutex;
	std::condition_variable stStopCondition;
public:
	bool Init();
	// 多线程把协程句柄放进运行队列并叫醒一个工人。不分配 CoroTask。
	void PostCoroutine(std::coroutine_handle<> stHandleParam);
	template<typename F>
	typename std::enable_if<!std::is_base_of<Task, typename std::remove_pointer<typename std::decay<F>::type>::type>::value, bool>::type
	AddTask(F&& fnFunctionParam, PRIO_ENUM iPriorityParam = PRIO_NORMAL)
	{
		typedef typename std::decay<F>::type Fn;
		return AddTask(new FnTask<Fn>(std::forward<F>(fnFunctionParam), iPriorityParam));
	}
	void StartWork();
	// 单线程主循环及同步适配器共用：普通任务、协程、IO 都必须推进。
	void PumpOnce();
	// 打退出标记并叫醒各线程。线程看到标记自己返回，主线程在 StartWork 里 join，再回到 main 析构。
	// SIGINT、SIGTERM、SIGQUIT 由 poll 接住后也走这里，退出码为 0。
	void RequestStop(int iCodeParam);
	bool Stopping() const { return bStop.load(); }
	int32_t ExitCode() const { return iExitCode; }
	void SetDrain(bool (*fnFunctionParam)());
	// 单线程这一轮没跑到、但已经挂进协程队列的。yield(0) 靠它留到下一轮。
	void SetPending(bool (*fnFunctionParam)());
	// 协程 resume 返回后调用。用来释放已经跑完的根协程，不在 base 里直接依赖 proto。
	void SetAfterResume(void (*fnFunctionParam)());

	// 线程交给 out，由调用方在退出时 join。
	static bool CreateSpecThread(std::thread& stOutputParam, void* (*fnThreadHandleParam)(void*), void* pArgumentParam);
	static void InitIgnoreSig();
	static void InitUnIgnoreSig(int iSigParam);
private:
	bool AddTask(Task* pTaskParam);
	void SingleThreadStartWork();
	void MultiThreadStartWork();
};
}}
