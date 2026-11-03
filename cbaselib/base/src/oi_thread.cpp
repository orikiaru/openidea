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
static std::string GetPrioConfigString(PRIO_ENUM iPriorityParam)
{
	switch (iPriorityParam)
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
	for(int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
	{
		ThreadData& stThreadData = vThreadData[iIndexLocal];
		stThreadData.iThreadCount = 1;
	}
#else
	for(int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
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

Task* FetchTask(ThreadPool::ThreadData* pDataParam)
{
	assert(pDataParam);
	Task* pTask = NULL;
	if(pDataParam->iTaskCount)
	{
		pTask = pDataParam->vTaskList.front();
		pDataParam->vTaskList.pop_front();
		pDataParam->iTaskCount --;
	}

	return pTask;
}

static void WorkerMain(ThreadPool::ThreadData* pDataParam)
{
	ThreadPool::InitIgnoreSig();
	assert(pDataParam);
	unsigned iCoroutineStreak = 0;
	for (;;)
	{
		Task* pTask = NULL;
		std::coroutine_handle<> stCoroutine;
		{
			std::unique_lock<std::mutex> stLock(pDataParam->stMutex);
			pDataParam->iIdleThreadCount++;
			pDataParam->stCondition.wait(stLock, [&]() {
				return pDataParam->iTaskCount > 0 || pDataParam->iCoroutineCount > 0 || ThreadPool::GetInstance().Stopping();
			});
			pDataParam->iIdleThreadCount--;
			// 停了就走。否则 poll 上的定时器会一直塞任务，join 等不到。
			if (ThreadPool::GetInstance().Stopping())
			{
				return;
			}
			if (pDataParam->iCoroutineCount > 0 && (pDataParam->iTaskCount == 0 || iCoroutineStreak < 32))
			{
				stCoroutine = pDataParam->vCoroutines.front();
				pDataParam->vCoroutines.pop_front();
				pDataParam->iCoroutineCount--;
				if (iCoroutineStreak < 32) ++iCoroutineStreak;
			}
			else
			{
				iCoroutineStreak = 0;
				pTask = pDataParam->vTaskList.front();
				pDataParam->vTaskList.pop_front();
				pDataParam->iTaskCount--;
			}
		}
		if (stCoroutine)
		{
			// 只 resume。跑完的根协程由 SetAfterResume 登记的函数释放。
			stCoroutine.resume();
			if (ThreadPool::GetInstance().fnAfterResume)
			{
				ThreadPool::GetInstance().fnAfterResume();
			}
		}
		else
		{
			pTask->Exec();
		}
	}
}

void ThreadPool::PostCoroutine(std::coroutine_handle<> stHandleParam)
{
	if (!stHandleParam)
	{
		return;
	}
#ifndef _SINGLE_THREAD_
	ThreadData& stThreadData = vThreadData[PRIO_NORMAL];
	bool bWake = false;
	{
		std::lock_guard<std::mutex> stLock(stThreadData.stMutex);
		stThreadData.vCoroutines.push_back(stHandleParam);
		stThreadData.iCoroutineCount++;
		bWake = stThreadData.iIdleThreadCount > 0;
	}
	if (bWake)
	{
		stThreadData.stCondition.notify_one();
	}
#else
	(void)stHandleParam;
#endif
}

void ThreadPool::RequestStop(int iCodeParam)
{
	{
		std::lock_guard<std::mutex> stLock(stStopMutex);
		if (!bStop.exchange(true))
		{
			iExitCode = iCodeParam;
		}
	}
	stStopCondition.notify_one();
	for (int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
	{
		// 与 wait 的谓词检查握手，避免检查 false 后、真正入睡前漏掉通知。
		{ std::lock_guard<std::mutex> stLock(vThreadData[iIndexLocal].stMutex); }
		vThreadData[iIndexLocal].stCondition.notify_all();
	}
}

bool ThreadPool::CreateSpecThread(std::thread& stOutputParam, void* (*fnThreadHandleParam)(void*), void* pArgumentParam)
{
#ifndef _SINGLE_THREAD_
	InitIgnoreSig();
	try
	{
		// 新线程等 out 写好再跑。线程函数里常拿自己的 id 和 out.get_id() 比。
		std::promise<void> stGo;
		std::shared_future<void> stStarted = stGo.get_future().share();
		std::thread stWorker([fnThreadHandleParam, pArgumentParam, stStarted]() {
			stStarted.wait();
			fnThreadHandleParam(pArgumentParam);
		});
		stOutputParam = std::move(stWorker);
		stGo.set_value();
	}
	catch (...)
	{
		return false;
	}
	return true;
#else
	(void)stOutputParam;
	(void)fnThreadHandleParam;
	(void)pArgumentParam;
	assert(false && "ThreadPool::CreateSpecThread is singlethread");
	return false;
#endif
}

void ThreadPool::SetDrain(bool (*fnFunctionParam)())
{
	fnDrain = fnFunctionParam;
}

void ThreadPool::SetPending(bool (*fnFunctionParam)())
{
	fnPending = fnFunctionParam;
}

void ThreadPool::SetAfterResume(void (*fnFunctionParam)())
{
	fnAfterResume = fnFunctionParam;
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
			pTask->Exec();
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
	while (!Stopping()) PumpOnce();
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
		stStopCondition.wait(stLock, [&]() { return bStop.load(); });
	}
	for (int iIndexLocal = PRIO_NORMAL; iIndexLocal < PRIO_NUM; ++iIndexLocal)
	{
		// 与 wait 的谓词检查握手，避免检查 false 后、真正入睡前漏掉通知。
		{ std::lock_guard<std::mutex> stLock(vThreadData[iIndexLocal].stMutex); }
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

bool ThreadPool::AddTask(Task* pTaskParam)
{
	assert(pTaskParam);
	int iPriorityLocal = pTaskParam->iPriority;
	assert(iPriorityLocal >= PRIO_NORMAL && iPriorityLocal < PRIO_NUM);
#ifdef _SINGLE_THREAD_
	ThreadData& stThreadData = vThreadData[iPriorityLocal];
	if(stThreadData.iTaskCount >= iMaxTaskCount)
	{
		SAFE_DELETE(pTaskParam);
		return false;
	}
	stThreadData.iTaskCount ++;
	stThreadData.vTaskList.push_back(pTaskParam);
#else
	ThreadData& stThreadData = vThreadData[iPriorityLocal];
	std::lock_guard<std::mutex> stLock(stThreadData.stMutex);
	if(stThreadData.iTaskCount >= iMaxTaskCount)
	{
		SAFE_DELETE(pTaskParam);
		return false;
	}
	stThreadData.iTaskCount ++;
	stThreadData.vTaskList.push_back(pTaskParam);
	if(stThreadData.iTaskCount == 1)
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


void ThreadPool::InitUnIgnoreSig(int iSigParam)
{
	std::signal(iSigParam, SIG_DFL);
}







}}
