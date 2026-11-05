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

namespace OI
{
namespace Thread
{
namespace
{
std::atomic<TaskErrorHandler> g_fnTaskErrorHandler{nullptr};
}
void SetTaskErrorHandler(TaskErrorHandler fnHandlerLocal)
{
	g_fnTaskErrorHandler.store(fnHandlerLocal);
}
void ReportTaskError(std::exception_ptr stErrorLocal) noexcept
{
	if (auto fnHandler = g_fnTaskErrorHandler.load())
	{
		try
		{
			fnHandler(stErrorLocal);
			return;
		}
		catch (...)
		{
			fprintf(stderr, "task error handler threw\n");
		}
	}
	try
	{
		std::rethrow_exception(stErrorLocal);
	}
	catch (const std::exception& stError)
	{
		fprintf(stderr, "task failed: %s\n", stError.what());
	}
	catch (...)
	{
		fprintf(stderr, "task failed: unknown exception\n");
	}
}
static void ExecuteTask(Task* pTask) noexcept
{
	try
	{
		pTask->Exec();
	}
	catch (...)
	{
		ReportTaskError(std::current_exception());
	}
}

static std::string GetPrioConfigString(PRIO_ENUM iPriority)
{
	switch (iPriority)
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

static constexpr int kDefaultThreadCount = 3;
bool ThreadPool::Init()
{
	int64_t iMaxTaskCountLocal = 0;
	if (TryGetConfig("ThreadPool.max_task_count", iMaxTaskCountLocal))
	{
		if (iMaxTaskCountLocal <= 0 || iMaxTaskCountLocal > (int64_t)UINT32_MAX)
		{
			fprintf(stderr, "ThreadPool.max_task_count=%lld must be in [1, %u]\n", (long long)iMaxTaskCountLocal, UINT32_MAX);
			return false;
		}
		iMaxTaskCount = (uint32_t)iMaxTaskCountLocal;
	}
#ifdef _SINGLE_THREAD_
	for (int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
	{
		ThreadData& stThreadData = vThreadData[iIndexLocal];
		stThreadData.iThreadCount = 1;
	}
#else
	for (int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
	{
		ThreadData& stThreadData = vThreadData[iIndexLocal];

		stThreadData.iThreadCount = kDefaultThreadCount;
		std::string sConfigKey = "ThreadPool." + GetPrioConfigString((PRIO_ENUM)iIndexLocal);
		TryGetConfigAndReturnFalse(sConfigKey, stThreadData.iThreadCount);

		if (iIndexLocal == PRIO_ENUM::PRIO_QUEUE)
		{
			stThreadData.iThreadCount = 1;
		}
	}
#endif
	return true;
}

Task* FetchTask(ThreadPool::ThreadData* pData)
{
	assert(pData);
	Task* pTask = NULL;
	if (pData->iTaskCount)
	{
		pTask = pData->vTaskList.front();
		pData->vTaskList.pop_front();
		pData->iTaskCount--;
	}

	return pTask;
}

static void WorkerMain(ThreadPool::ThreadData* pData)
{
	ThreadPool::InitIgnoreSig();
	assert(pData);
	unsigned iCoroutineStreak = 0;
	for (;;)
	{
		Task* pTask = NULL;
		std::coroutine_handle<> stCoroutine;
		{
			std::unique_lock<std::mutex> stLock(pData->stMutex);
			pData->iIdleThreadCount++;
			pData->stCondition.wait(stLock, [&]()
			{
				return pData->iTaskCount > 0 || pData->iCoroutineCount > 0 || ThreadPool::GetInstance().Stopping();
			});
			pData->iIdleThreadCount--;
			// 停了就走。否则 poll 上的定时器会一直塞任务，join 等不到。
			if (ThreadPool::GetInstance().Stopping())
			{
				return;
			}
			if (pData->iCoroutineCount > 0 && (pData->iTaskCount == 0 || iCoroutineStreak < 32))
			{
				stCoroutine = pData->vCoroutines.front();
				pData->vCoroutines.pop_front();
				pData->iCoroutineCount--;
				if (iCoroutineStreak < 32)
				{
					++iCoroutineStreak;
				}
			}
			else
			{
				iCoroutineStreak = 0;
				pTask = pData->vTaskList.front();
				pData->vTaskList.pop_front();
				pData->iTaskCount--;
			}
		}
		if (stCoroutine)
		{
			// 只 resume。跑完的根协程由 SetResume 登记的函数释放。
			if (ThreadPool::GetInstance().fnResume)
			{
				ThreadPool::GetInstance().fnResume(stCoroutine);
			}
			else
			{
				stCoroutine.resume();
			}
		}
		else
		{
			ExecuteTask(pTask);
		}
	}
}

void ThreadPool::PostCoroutine(std::coroutine_handle<> stHandle)
{
	// 协程唤醒只进 normal 队列。多个等待者各投一次，工人取到才 resume，不在投递线程上跑。
	if (!stHandle)
	{
		return;
	}
#ifndef _SINGLE_THREAD_
	ThreadData& stThreadData = vThreadData[PRIO_NORMAL];
	bool bWake = false;
	{
		std::lock_guard<std::mutex> stLock(stThreadData.stMutex);
		stThreadData.vCoroutines.push_back(stHandle);
		stThreadData.iCoroutineCount++;
		bWake = stThreadData.iIdleThreadCount > 0;
	}
	if (bWake)
	{
		stThreadData.stCondition.notify_one();
	}
#else
	(void)stHandle;
#endif
}

bool ThreadPool::AddStopHandler(void (*fnHandlerLocal)())
{
	::OI::Thread::MutexGuard stLock(m_stStopHandlerMutex);
	if (!fnHandlerLocal || m_bStopRequested.load())
	{
		return false;
	}
	for (const auto& fnHandler : m_vStopHandlers)
	{
		if (fnHandler == fnHandlerLocal)
		{
			return true;
		}
	}
	m_vStopHandlers.push_back(fnHandlerLocal);
	return true;
}

void ThreadPool::RequestStop(int iCode)
{
	{
		::OI::Thread::MutexGuard stLock(m_stStopHandlerMutex);
		if (m_bStopRequested.exchange(true))
		{
			return;
		}
		iExitCode = iCode;
	}
	::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		static_cast<ThreadPool*>(pArgument)->FinishStop();
	}, this);
}

void ThreadPool::FinishStop()
{
	// 请求停止后不再接受注册；回调与网络完成串行，不能靠已停工人的超时任务解锁。
	for (const auto& fnHandler : m_vStopHandlers)
	{
		try
		{
			fnHandler();
		}
		catch (...)
		{
			ReportTaskError(std::current_exception());
		}
	}
	{
		std::lock_guard<std::mutex> stLock(stStopMutex);
		bStop.store(true);
	}
	stStopCondition.notify_one();
	for (int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
	{
		// 与 wait 的谓词检查握手，避免检查 false 后、真正入睡前漏掉通知。
		{
			std::lock_guard<std::mutex> stLock(vThreadData[iIndexLocal].stMutex);
		}
		vThreadData[iIndexLocal].stCondition.notify_all();
	}
}

bool ThreadPool::CreateSpecThread(std::thread& stOutput, void* (*fnThreadHandle)(void*), void* pArgument)
{
#ifndef _SINGLE_THREAD_
	InitIgnoreSig();
	try
	{
		// 新线程等 out 写好再跑。线程函数里常拿自己的 id 和 out.get_id() 比。
		std::promise<void> stGo;
		std::shared_future<void> stStarted = stGo.get_future().share();
		std::thread stWorker([fnThreadHandle, pArgument, stStarted]()
		{
			stStarted.wait();
			fnThreadHandle(pArgument);
		});
		stOutput = std::move(stWorker);
		stGo.set_value();
	}
	catch (...)
	{
		return false;
	}
	return true;
#else
	(void)stOutput;
	(void)fnThreadHandle;
	(void)pArgument;
	assert(false && "ThreadPool::CreateSpecThread is singlethread");
	return false;
#endif
}

void ThreadPool::SetDrain(bool (*fnFunction)())
{
	fnDrain = fnFunction;
}

void ThreadPool::SetPending(bool (*fnFunction)())
{
	fnPending = fnFunction;
}

void ThreadPool::SetResume(void (*fnFunction)(std::coroutine_handle<>))
{
	fnResume = fnFunction;
}

void ThreadPool::PumpOnce()
{
#ifdef _SINGLE_THREAD_
	// 普通 Task 也只跑此刻队列里的。执行中再投的留到 poll 之后，避免自己把 poll 饿死。
	for (int iIndexLocal = PRIO_NUM - 1; iIndexLocal >= PRIO_NORMAL; --iIndexLocal)
	{
		ThreadData& stThreadData = vThreadData[iIndexLocal];
		unsigned int iNumber = stThreadData.iTaskCount;
		for (unsigned int iItemIndex = 0; iItemIndex < iNumber; ++iItemIndex)
		{
			Task* pTask = FetchTask(&stThreadData);
			if (!pTask)
			{
				break;
			}
			ExecuteTask(pTask);
		}
	}
	if (fnDrain)
	{
		fnDrain();
	}
	bool bPending = fnPending && fnPending();
	if (!bPending)
	{
		for (int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
		{
			if (vThreadData[iIndexLocal].iTaskCount)
			{
				bPending = true;
				break;
			}
		}
	}
	// 队列里还有下一轮，就只试一次网络；谁都不跑了才堵住等。
	::OI::Net::Poll::GetInstance().PollWait(bPending ? 0 : 1000);
#else
	assert(false && "PumpOnce is single-thread only");
#endif
}

void ThreadPool::SingleThreadStartWork()
{
	InitIgnoreSig();
	while (!Stopping())
	{
		PumpOnce();
	}
}

void ThreadPool::MultiThreadStartWork()
{
#ifndef _SINGLE_THREAD_
	//以上是主线程需要先忽略掉的信号
	ThreadPool::InitIgnoreSig();
	std::vector<std::thread> vWorkers;
	for (int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
	{
		ThreadData& stThreadData = vThreadData[iIndexLocal];
		for (int iNumber = 0; iNumber < stThreadData.iThreadCount; ++iNumber)
		{
			vWorkers.emplace_back(WorkerMain, &stThreadData);
		}
	}

	{
		std::unique_lock<std::mutex> stLock(stStopMutex);
		stStopCondition.wait(stLock, [&]()
		{
			return bStop.load();
		});
	}
	for (int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
	{
		// 与 wait 的谓词检查握手，避免检查 false 后、真正入睡前漏掉通知。
		{
			std::lock_guard<std::mutex> stLock(vThreadData[iIndexLocal].stMutex);
		}
		vThreadData[iIndexLocal].stCondition.notify_all();
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vWorkers.size(); ++iIndexLocal)
	{
		vWorkers[iIndexLocal].join();
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
	int iPriorityLocal = pTask->iPriority;
	assert(iPriorityLocal >= PRIO_NORMAL && iPriorityLocal < PRIO_NUM);
#ifdef _SINGLE_THREAD_
	ThreadData& stThreadData = vThreadData[iPriorityLocal];
	if (stThreadData.iTaskCount >= iMaxTaskCount)
	{
		SAFE_DELETE(pTask);
		return false;
	}
	stThreadData.iTaskCount++;
	stThreadData.vTaskList.push_back(pTask);
#else
	ThreadData& stThreadData = vThreadData[iPriorityLocal];
	std::lock_guard<std::mutex> stLock(stThreadData.stMutex);
	if (stThreadData.iTaskCount >= iMaxTaskCount)
	{
		SAFE_DELETE(pTask);
		return false;
	}
	stThreadData.iTaskCount++;
	stThreadData.vTaskList.push_back(pTask);
	if (stThreadData.iTaskCount == 1)
	{
		stThreadData.stCondition.notify_all();
	}
#endif
	return true;
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

void ThreadPool::InitUnIgnoreSig(int iSig)
{
	std::signal(iSig, SIG_DFL);
}

}
}
