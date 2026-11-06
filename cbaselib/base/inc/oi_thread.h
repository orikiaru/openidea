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
	// RequestStop 已调用。之后 AddStopHandler 拒绝注册。
	std::atomic<bool> m_bStopRequested{false};
	// 保护停止回调列表。
	::OI::Thread::Mutex m_stStopHandlerMutex;
	// 停止时在 poll 线程执行，回调不可阻塞。
	std::vector<void (*)()> m_vStopHandlers;
	// 跑完停止回调，置 bStop 并唤醒卡在 StartWork 里的线程。
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
		// 保护运行队列和空闲计数。
		::OI::Thread::Mutex stMutex;
		// 没有协程可跑时，空闲工人睡在这里。
		::OI::Thread::MutexCond stCondition;
	} stThreadData;

	// 单线程每轮先排空已经在队列里的协程。由协程运行时注册。
	bool (*fnDrain)() = nullptr;
	// 队列里还有留到下一轮的协程时返回真。PumpOnce 据此不堵在 poll 上。
	bool (*fnPending)() = nullptr;
	// 工人 resume 协程的入口。运行时用来标记正在跑协程，并释放跑完的根协程。
	void (*fnResume)(std::coroutine_handle<>) = nullptr;
	// 停止回调已经跑完。工人看到后退出，StartWork 被唤醒。
	std::atomic<bool> bStop{false};
	// RequestStop 写入，RunFramework 把它交回 main。
	int32_t iExitCode = 0;
	// StartWork 拿着它等停止完成。
	::OI::Thread::Mutex stStopMutex;
	// 停止回调跑完后 Signal，叫醒 StartWork。
	::OI::Thread::MutexCond stStopCondition;

  public:
	// 读 [ThreadPool] threadcount。单线程固定 1。键缺失、类型不对或值不大于 0 时返回 false。
	bool Init();
	// 多线程把协程句柄放进运行队列并叫醒一个工人。不分配 CoroTask。
	void PostCoroutine(std::coroutine_handle<> stHandle);
	// 进入主循环直到停止。单线程在调用线程里 PumpOnce；多线程拉起工人并等它们退出。
	void StartWork();
	// 单线程主循环及同步适配器共用：协程和 IO 都必须推进。
	void PumpOnce();
	// 初始化时注册；停止开始后拒绝注册。回调在 poll 上执行且不可阻塞。
	bool AddStopHandler(void (*fnHandler)());
	// 已经 RequestStop，新的停止回调不再接受。
	bool StopRequested() const
	{
		return m_bStopRequested.load();
	}
	// 先关闭入口并执行停止回调，再置 Stopping 并通知工人，join 完最后停止 poll。
	// SIGINT、SIGTERM、SIGQUIT 也走这里，退出码为 0；重复请求不重复执行回调。
	void RequestStop(int iCode);
	// 停止回调已经跑完，工人可以退出。
	bool Stopping() const
	{
		return bStop.load();
	}
	// RunFramework 交回 main 的退出码。
	int32_t ExitCode() const
	{
		return iExitCode;
	}
	// 注册单线程每轮排空协程的函数。
	void SetDrain(bool (*fnFunction)());
	// 单线程这一轮没跑到、但已经挂进协程队列的。yield(0) 靠它留到下一轮。
	void SetPending(bool (*fnFunction)());
	// 工人通过它 resume 协程。协程运行时借此标记“正在跑协程”并释放跑完的根协程，不在 base 里直接依赖 proto。
	void SetResume(void (*fnFunction)(std::coroutine_handle<>));

	// 多线程开一条独立线程，句柄交给调用方 join。线程函数要等 stOutput 写好才开始跑。单线程直接断言失败。
	static bool CreateSpecThread(std::thread& stOutput, void* (*fnThreadHandle)(void*), void* pArgument);
	// 忽略 SIGPIPE 和 SIGHUP，避免对端关掉连接时把进程打死。
	static void InitIgnoreSig();
	// 把指定信号恢复成默认处理。
	static void InitUnIgnoreSig(int iSig);

  private:
	// 单线程主循环：反复 PumpOnce，直到 Stopping。
	void SingleThreadStartWork();
	// 多线程主循环：拉起 threadcount 个工人，等停止完成后 join，再停 poll。
	void MultiThreadStartWork();
};
}
}
