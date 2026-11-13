#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <chrono>
#include <csignal>
#include <future>
#include "oi_timer_manager.h"
#include "oi_poll.h"
#include "oi_blocking_work_session.h"
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
int32_t ThreadPool::Init()
{
#ifdef _SINGLE_THREAD_
	stThreadData.iThreadCount = 1;
#else
	// 只读工人数。旧配置里的 urgent、queue、max_task_count 已无对应队列，保留也不报错。
	if (!TryGetConfig("ThreadPool.threadcount", stThreadData.iThreadCount))
	{
		// normal 是改名前的键，留着兼容老配置，读到了提示一次改名。
		if (!TryGetConfig("ThreadPool.normal", stThreadData.iThreadCount))
		{
			fprintf(stderr, "ThreadPool.threadcount missing or not an integer; set [ThreadPool] threadcount = <worker count>\n");
			fflush(stderr);
			return OI_ERR_INVALID_ARGUMENT;
		}
		fprintf(stderr, "ThreadPool.normal is deprecated, rename it to ThreadPool.threadcount\n");
		fflush(stderr);
	}
	if (stThreadData.iThreadCount <= 0)
	{
		fprintf(stderr, "ThreadPool.threadcount=%d must be positive\n", stThreadData.iThreadCount);
		fflush(stderr);
		return OI_ERR_INVALID_ARGUMENT;
	}
#endif
	return OI_OK;
}

static void WorkerMain(ThreadPool::ThreadData* pData)
{
	ThreadPool::InitIgnoreSig();
	assert(pData);
	for (;;)
	{
		std::coroutine_handle<> stCoroutine;
		{
			::OI::Thread::MutexGuard stLock(pData->stMutex);
			pData->iIdleThreadCount++;
			while (pData->vCoroutines.empty() && !ThreadPool::GetInstance().Stopping())
			{
				pData->stCondition.Wait(pData->stMutex);
			}
			pData->iIdleThreadCount--;
			// 停了就走。否则 poll 上的定时器会一直投协程，join 等不到。
			if (ThreadPool::GetInstance().Stopping())
			{
				return;
			}
			stCoroutine = pData->vCoroutines.front();
			pData->vCoroutines.pop_front();
		}
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
}

void ThreadPool::PostCoroutine(std::coroutine_handle<> stHandle)
{
	// 多个等待者各投一次，工人取到才 resume，不在投递线程上跑。
	if (!stHandle)
	{
		return;
	}
#ifndef _SINGLE_THREAD_
	bool bWake = false;
	{
		::OI::Thread::MutexGuard stLock(stThreadData.stMutex);
		stThreadData.vCoroutines.push_back(stHandle);
		bWake = stThreadData.iIdleThreadCount > 0;
	}
	if (bWake)
	{
		stThreadData.stCondition.Signal();
	}
#else
	(void)stHandle;
#endif
}

int32_t ThreadPool::AddStopHandler(void (*fnHandlerLocal)())
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("ThreadPool::AddStopHandler");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	if (!fnHandlerLocal)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	::OI::Thread::MutexGuard stLock(m_stStopHandlerMutex);
	if (m_bStopRequested.load())
	{
		return OI_ERR_CONNECT;
	}
	for (const auto& fnHandler : m_vStopHandlers)
	{
		if (fnHandler == fnHandlerLocal)
		{
			return OI_OK;
		}
	}
	m_vStopHandlers.push_back(fnHandlerLocal);
	return OI_OK;
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
		::OI::Net::BlockingWorkSessionManager::GetInstance().Drain([](void* pArgumentLocal)
		{
			static_cast<ThreadPool*>(pArgumentLocal)->FinishStop();
		}, pArgument);
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
		::OI::Thread::MutexGuard stLock(stStopMutex);
		bStop.store(true);
	}
	stStopCondition.Signal();
	// 与 Wait 前的条件检查握手，避免检查 false 后、真正入睡前漏掉通知。
	{
		::OI::Thread::MutexGuard stLock(stThreadData.stMutex);
	}
	stThreadData.stCondition.Broadcast();
}

int32_t ThreadPool::CreateSpecThread(std::thread& stOutput, void* (*fnThreadHandle)(void*), void* pArgument)
{
	if (!fnThreadHandle)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
#ifndef _SINGLE_THREAD_
	if (stOutput.joinable())
	{
		return OI_ERR_BUSY;
	}
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
	catch (const std::exception& stError)
	{
		fprintf(stderr, "CreateSpecThread failed: %s\n", stError.what());
		return OI_ERR_FAIL;
	}
	catch (...)
	{
		fprintf(stderr, "CreateSpecThread failed: unknown exception\n");
		return OI_ERR_FAIL;
	}
	return OI_OK;
#else
	(void)stOutput;
	(void)fnThreadHandle;
	(void)pArgument;
	fprintf(stderr, "CreateSpecThread is not supported in single-thread mode\n");
	return OI_ERR_INVALID_ARGUMENT;
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
	if (fnDrain)
	{
		fnDrain();
	}
	const bool bPending = fnPending && fnPending();
	// 协程队列里还有下一轮，就只试一次网络；谁都不跑了才堵住等。
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
	for (int iNumber = 0; iNumber < stThreadData.iThreadCount; ++iNumber)
	{
		vWorkers.emplace_back(WorkerMain, &stThreadData);
	}

	{
		::OI::Thread::MutexGuard stLock(stStopMutex);
		while (!bStop.load())
		{
			stStopCondition.Wait(stStopMutex);
		}
	}
	// 与 Wait 前的条件检查握手，避免检查 false 后、真正入睡前漏掉通知。
	{
		::OI::Thread::MutexGuard stLock(stThreadData.stMutex);
	}
	stThreadData.stCondition.Broadcast();
	for (auto& stWorker : vWorkers)
	{
		stWorker.join();
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
