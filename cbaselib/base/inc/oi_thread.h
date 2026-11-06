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
#include <list>
#include <memory>
#include <type_traits>
#include <utility>
#include "oi_config.h"
#include "oi_singleton.h"
#include "oi_lock.h"
#include <cstdint>

namespace OI
{
namespace Thread
{
// 协程、定时器、停止回调等调度入口的异常统一报到这里；处理器不得抛异常，nullptr 恢复 stderr 报告。
using TaskErrorHandler = void (*)(std::exception_ptr);
void SetTaskErrorHandler(TaskErrorHandler fnHandler);
void ReportTaskError(std::exception_ptr stError) noexcept;

class ThreadPool
{
	DECLARE_SINGLETON(ThreadPool)
	ThreadPool() = default;
	std::atomic<bool> m_bStopRequested{false};
	::OI::Thread::Mutex m_stStopHandlerMutex;
	std::vector<void (*)()> m_vStopHandlers;
	void FinishStop();

  public:
	struct ThreadData
	{
		// 协程句柄运行队列。工人取到就 resume，挂起后线程立刻去拿下一个。
		std::deque<std::coroutine_handle<>> vCoroutines;
		// 正睡在 stCondition 上的工人数。为 0 时投递不用 notify，省一次 futex。
		int32_t iIdleThreadCount = 0;
		// 多线程工人数，配置 [ThreadPool] threadcount。
		int32_t iThreadCount = 0;
		::OI::Thread::Mutex stMutex;
		::OI::Thread::MutexCond stCondition;
	} stThreadData;

	bool (*fnDrain)() = nullptr;
	bool (*fnPending)() = nullptr;
	void (*fnResume)(std::coroutine_handle<>) = nullptr;
	std::atomic<bool> bStop{false};
	int32_t iExitCode = 0;
	::OI::Thread::Mutex stStopMutex;
	::OI::Thread::MutexCond stStopCondition;

  public:
	bool Init();
	// 多线程把协程句柄放进运行队列并叫醒一个工人。不分配 CoroTask。
	void PostCoroutine(std::coroutine_handle<> stHandle);
	void StartWork();
	// 单线程主循环及同步适配器共用：协程和 IO 都必须推进。
	void PumpOnce();
	// 初始化时注册；停止开始后拒绝注册。回调在 poll 上执行且不可阻塞。
	bool AddStopHandler(void (*fnHandler)());
	bool StopRequested() const
	{
		return m_bStopRequested.load();
	}
	// 先关闭入口并执行停止回调，再置 Stopping 并通知工人，join 完最后停止 poll。
	// SIGINT、SIGTERM、SIGQUIT 也走这里，退出码为 0；重复请求不重复执行回调。
	void RequestStop(int iCode);
	bool Stopping() const
	{
		return bStop.load();
	}
	int32_t ExitCode() const
	{
		return iExitCode;
	}
	void SetDrain(bool (*fnFunction)());
	// 单线程这一轮没跑到、但已经挂进协程队列的。yield(0) 靠它留到下一轮。
	void SetPending(bool (*fnFunction)());
	// 工人通过它 resume 协程。协程运行时借此标记“正在跑协程”并释放跑完的根协程，不在 base 里直接依赖 proto。
	void SetResume(void (*fnFunction)(std::coroutine_handle<>));

	// 线程交给 out，由调用方在退出时 join。
	static bool CreateSpecThread(std::thread& stOutput, void* (*fnThreadHandle)(void*), void* pArgument);
	static void InitIgnoreSig();
	static void InitUnIgnoreSig(int iSig);

  private:
	void SingleThreadStartWork();
	void MultiThreadStartWork();
};
}
}
