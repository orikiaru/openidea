#include "oi_errno.h"
#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <stdlib.h>
#include <atomic>
#include <deque>
#include <unordered_map>
#include <memory>
#include "oi_lock.h"
#ifndef _WIN32
#include <chrono>
#include <thread>
#endif
#include <uv.h>
#include "oi_poll.h"
#include "oi_channel.h"
#include "oi_fifo_channel.h"
#include "oi_thread.h"
#include "oi_header.h"
#include "oi_net_os.h"
#include "oi_blocking_work_session.h"
namespace OI
{
namespace Net
{

namespace
{
struct EventChange
{
	int32_t iEnable;
	int32_t iDisable;
};

struct IoWatch
{
	uv_poll_t stPoll{};
	uv_timer_t* pTimer = nullptr;
	uv_timer_t* pResumeTimer = nullptr;
	int32_t iPausedMask = 0;
	std::atomic<Channel*> pChannel{nullptr};
	net_socket_t iDescriptor = INVALID_SOCKET_ID;
	int32_t iMask = 0;
	// libuv 借用内存到 close 回调。排队的请求也各持一份。
	std::shared_ptr<IoWatch> pClosing;
#ifndef _SINGLE_THREAD_
	std::vector<EventChange> vRequests;
	bool bCloseDescriptor = false;
	bool bRemoved = false;
#endif
};

enum IoOpType
{
	IO_OP_MOD,
	IO_OP_DEL,
	IO_OP_TICK,
	IO_OP_CALL,
};

struct IoOp
{
	IoOpType iType{};
	Channel* pChannel = nullptr;
	std::shared_ptr<IoWatch> pWatch;
	void (*fnCall)(void*) = nullptr;
	void* pArgument = nullptr;
	void (*fnTick)() = nullptr;
	unsigned iTickMilliseconds = 0;
};

int MaskToUv(int32_t iMask)
{
	return ((iMask & IO_EV_READ) ? UV_READABLE : 0) | ((iMask & IO_EV_WRITE) ? UV_WRITABLE : 0);
}

void OnPoll(uv_poll_t* pHandle, int iStatus, int iEvents)
{
	auto* pWatch = static_cast<IoWatch*>(pHandle->data);
	if (iStatus < 0 || (iEvents & (UV_READABLE | UV_DISCONNECT)))
	{
		if (auto* pChannel = pWatch->pChannel.load())
		{
			pChannel->DataIn();
		}
	}
	if (iEvents & UV_WRITABLE)
	{
		if (auto* pChannel = pWatch->pChannel.load())
		{
			pChannel->DataOut();
		}
	}
	if (auto* pChannel = pWatch->pChannel.load())
	{
		pChannel->TryClose();
	}
}

void CloseWatchOnLoop(const std::shared_ptr<IoWatch>& pWatch, bool bCloseDescriptor)
{
	if (!bCloseDescriptor)
	{
		pWatch->iDescriptor = INVALID_SOCKET_ID;
	}
	for (auto* pTimer : {pWatch->pTimer, pWatch->pResumeTimer})
	{
		if (pTimer)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(pTimer), [](uv_handle_t* pHandle)
			{
				delete reinterpret_cast<uv_timer_t*>(pHandle);
			});
		}
	}
	pWatch->pClosing = pWatch;
	uv_close(reinterpret_cast<uv_handle_t*>(&pWatch->stPoll), [](uv_handle_t* pHandle)
	{
		auto* pWatchLocal = static_cast<IoWatch*>(pHandle->data);
		NetOs_Close(pWatchLocal->iDescriptor);
		pWatchLocal->pClosing.reset();
	});
}

// 只在 IO 线程改当前 mask 和 libuv 监听。
void SetEvents(IoWatch& stWatch, int32_t iMask)
{
	auto* pChannel = stWatch.pChannel.load();
	if (!pChannel || iMask == stWatch.iMask)
	{
		return;
	}
	const int iStatus = iMask ? uv_poll_start(&stWatch.stPoll, MaskToUv(iMask), OnPoll) : uv_poll_stop(&stWatch.stPoll);
	if (iStatus != 0)
	{
		fprintf(stderr, "poll modify failed mask=%d: %s (%d)\n", iMask, uv_strerror(iStatus), iStatus);
		return;
	}
	stWatch.iMask = iMask;
	pChannel->iEventMask = iMask;
}
}

struct Poll::State
{
	uv_loop_t stLoop{};
	class WakeupChannel final : public FifoChannel
	{
		State& m_stState;

	  public:
		explicit WakeupChannel(State& stState)
		    : FifoChannel(nullptr), m_stState(stState)
		{
		}
		using FifoChannel::OpenPair;
		void DataIn() override
		{
			bool bHealthy;
			{
				::OI::Thread::NativeMutexGuard stGuard(m_stState.stQueueLock);
				bHealthy = ReadWakeup();
			}
			m_stState.Drain();
			if (!bHealthy)
			{
				::OI::Thread::ThreadPool::GetInstance().RequestStop(OI_ERR_SOCKET);
			}
		}
	};
	std::unique_ptr<WakeupChannel> pWakeup;
	int32_t iCloseStatus = 0;
#ifndef _SINGLE_THREAD_
	::OI::Thread::NativeMutexCond stStarted;
	bool bStartDone = false;
	int32_t iStartStatus = OI_ERR_FAIL;
#endif
	uv_timer_t stTickTimer{};
	static constexpr int vSignals[] = {
	    SIGINT,
	    SIGTERM,
#ifdef SIGQUIT
	    SIGQUIT,
#endif
	};
	uv_signal_t vSignalHandles[sizeof(vSignals) / sizeof(vSignals[0])]{};

	// 单线程构建也有后台完成通知投递到此队列。
	::OI::Thread::NativeMutex stQueueLock;
	std::deque<IoOp> vOperations;
	QueuedCall* pCallHead = nullptr;
	QueuedCall* pCallTail = nullptr;
	bool bReady = false;
	std::atomic<bool> bStop{false};
	bool bTickArmed = false;
	unsigned iTickMilliseconds = 50;
	void (*fnTick)() = nullptr;
	unsigned iPollDepth = 0;

	// 只有 loop 增删。后台注销也须安全查表，单线程的掩码读写不取此锁。
	::OI::Thread::NativeMutex stWatchLock;
	std::unordered_map<Channel*, std::shared_ptr<IoWatch>> mWatches;
#ifndef _SINGLE_THREAD_
	::OI::Thread::NativeMutexCond stRemoved;
#endif

	void PostLocked(const IoOp& stOperation)
	{
		const bool bWake = vOperations.empty() && !pCallHead;
		vOperations.push_back(stOperation);
		if (bReady && bWake)
		{
			pWakeup->Wakeup();
		}
	}

	void Post(const IoOp& stOperation)
	{
		::OI::Thread::NativeMutexGuard stGuard(stQueueLock);
		PostLocked(stOperation);
	}

	void StartTick()
	{
		if (bTickArmed || !fnTick || !iTickMilliseconds)
		{
			return;
		}
		uv_timer_init(&stLoop, &stTickTimer);
		uv_timer_start(&stTickTimer, [](uv_timer_t* pTimer)
		{
			auto* pState = static_cast<State*>(pTimer->loop->data);
			if (pState->fnTick)
			{
				pState->fnTick();
			}
		}, iTickMilliseconds, iTickMilliseconds);
		bTickArmed = true;
	}

#ifndef _SINGLE_THREAD_
	// 先取走已入队的开关请求，再在 IO 线程按提交顺序执行。loop 本轮的请求排在它们后面。
	void ApplyPending(IoWatch& stWatch)
	{
		std::vector<EventChange> vRequests;
		{
			::OI::Thread::NativeMutexGuard stGuard(stWatchLock);
			vRequests.swap(stWatch.vRequests);
		}
		int32_t iMask = stWatch.iMask;
		for (const auto& stRequest : vRequests)
		{
			iMask = (iMask | stRequest.iEnable) & ~stRequest.iDisable;
		}
		SetEvents(stWatch, iMask);
	}

	void RemoveOnLoop(const IoOp& stOperation)
	{
		::OI::Thread::NativeMutexGuard stGuard(stWatchLock);
		auto it = mWatches.find(stOperation.pChannel);
		if (it != mWatches.end() && it->second == stOperation.pWatch)
		{
			mWatches.erase(it);
			CloseWatchOnLoop(stOperation.pWatch, stOperation.pWatch->bCloseDescriptor);
		}
		stOperation.pWatch->bRemoved = true;
		stRemoved.Broadcast();
	}
#endif

	void Drain()
	{
		std::deque<IoOp> vBatch;
		QueuedCall* pCalls;
		{
			::OI::Thread::NativeMutexGuard stGuard(stQueueLock);
			vBatch.swap(vOperations);
			pCalls = pCallHead;
			pCallHead = nullptr;
			pCallTail = nullptr;
		}
		for (const auto& stOperation : vBatch)
		{
			switch (stOperation.iType)
			{
			case IO_OP_TICK:
				fnTick = stOperation.fnTick;
				iTickMilliseconds = stOperation.iTickMilliseconds;
				StartTick();
				break;
			case IO_OP_CALL:
				if (stOperation.fnCall)
				{
					stOperation.fnCall(stOperation.pArgument);
				}
				break;
#ifndef _SINGLE_THREAD_
			case IO_OP_MOD:
				ApplyPending(*stOperation.pWatch);
				break;
			case IO_OP_DEL:
				RemoveOnLoop(stOperation);
				break;
#endif
			default:
				break;
			}
		}
		while (pCalls)
		{
			auto* pCall = pCalls;
			pCalls = pCall->pNext;
			pCall->pNext = nullptr;
			pCall->fnCall(pCall->pArgument);
		}
	}

	int32_t Prepare()
	{
		const int iLoopStatus = uv_loop_init(&stLoop);
		if (iLoopStatus != 0)
		{
			fprintf(stderr, "poll loop init failed: %s (%d)\n", uv_strerror(iLoopStatus), iLoopStatus);
			return OI_ERR_FAIL;
		}
		stLoop.data = this;
		pWakeup = std::make_unique<WakeupChannel>(*this);
		int32_t iStatus = pWakeup->OpenPair();
		if (iStatus == OI_OK)
		{
			iStatus = GetInstance().RegisterOnLoop(pWakeup.get(), IO_EV_READ);
		}
		if (iStatus != OI_OK)
		{
			pWakeup.reset();
			uv_run(&stLoop, UV_RUN_NOWAIT);
			iCloseStatus = uv_loop_close(&stLoop);
			return iStatus;
		}
		for (size_t iIndex = 0; iIndex < sizeof(vSignals) / sizeof(vSignals[0]); ++iIndex)
		{
			uv_signal_init(&stLoop, &vSignalHandles[iIndex]);
			uv_signal_start(&vSignalHandles[iIndex], [](uv_signal_t*, int)
			{
				::OI::Thread::ThreadPool::GetInstance().RequestStop(0);
			}, vSignals[iIndex]);
		}
		{
			::OI::Thread::NativeMutexGuard stGuard(stQueueLock);
			bReady = true;
		}
		Drain();
		return OI_OK;
	}

	// 停止后再无 IO 回调。仍登记的通道保留其 socket，已投递的注销按调用方指定的归属收尾。
	void Finish()
	{
		{
			::OI::Thread::NativeMutexGuard stGuard(stQueueLock);
			bReady = false;
		}
		Drain();
		pWakeup.reset();
		{
			::OI::Thread::NativeMutexGuard stGuard(stWatchLock);
			for (auto& [pChannel, pWatch] : mWatches)
			{
				(void)pChannel;
				pWatch->pChannel.store(nullptr);
#ifdef _SINGLE_THREAD_
				CloseWatchOnLoop(pWatch, false);
#else
				CloseWatchOnLoop(pWatch, pWatch->bCloseDescriptor);
				pWatch->bRemoved = true;
#endif
			}
			mWatches.clear();
#ifndef _SINGLE_THREAD_
			stRemoved.Broadcast();
#endif
		}
		if (bTickArmed)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(&stTickTimer), nullptr);
		}
		for (auto& stSignal : vSignalHandles)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(&stSignal), nullptr);
		}
		// 执行本实例各句柄的 close 回调。
		uv_run(&stLoop, UV_RUN_NOWAIT);
		iCloseStatus = uv_loop_close(&stLoop);
		if (iCloseStatus != 0)
		{
			fprintf(stderr, "poll loop close failed: %s (%d)\n", uv_strerror(iCloseStatus), iCloseStatus);
		}
	}
};

Poll::Poll()
    : m_pState(std::make_unique<State>())
{
}

void Poll::PostOnLoop(QueuedCall& stCall) noexcept
{
	auto& stState = *m_pState;
	::OI::Thread::NativeMutexGuard stGuard(stState.stQueueLock);
	const bool bWake = stState.vOperations.empty() && !stState.pCallHead;
	stCall.pNext = nullptr;
	if (stState.pCallTail)
	{
		stState.pCallTail->pNext = &stCall;
	}
	else
	{
		stState.pCallHead = &stCall;
	}
	stState.pCallTail = &stCall;
	if (stState.bReady && bWake)
	{
		stState.pWakeup->Wakeup();
	}
}

Poll::~Poll()
{
	StopAndJoin();
}

void* Poll::StartThreadPollWait(void* pArgument)
{
	auto* pPoll = static_cast<Poll*>(pArgument);
	pPoll->m_stLoopThread = std::this_thread::get_id();
	::OI::Thread::ThreadPool::InitIgnoreSig();
	NetOs_Startup();
	auto& stState = *pPoll->m_pState;
	const int32_t iStatus = stState.Prepare();
#ifndef _SINGLE_THREAD_
	{
		::OI::Thread::NativeMutexGuard stGuard(stState.stQueueLock);
		stState.iStartStatus = iStatus;
		stState.bStartDone = true;
		stState.stStarted.Signal();
	}
#endif
	if (iStatus != OI_OK)
	{
		return nullptr;
	}
	while (!stState.bStop.load())
	{
		uv_run(&stState.stLoop, UV_RUN_ONCE);
	}
	stState.Finish();
	return nullptr;
}

int32_t Poll::StopAndJoin()
{
	bool bStopNeeded;
	{
		::OI::Thread::NativeMutexGuard stGuard(m_pState->stQueueLock);
		bStopNeeded = m_pState->bReady && !m_pState->bStop.load();
	}
	if (bStopNeeded)
	{
		CallOnLoop([](void* pArgument)
		{
			BlockingWorkSessionManager::GetInstance().Drain([](void* pData)
			{
				auto* pState = static_cast<State*>(pData);
				::OI::Thread::NativeMutexGuard stGuard(pState->stQueueLock);
				pState->bStop.store(true);
				pState->pWakeup->Wakeup();
			}, pArgument);
		}, m_pState.get());
#ifdef _SINGLE_THREAD_
		while (!m_pState->bStop.load())
		{
			PollWait();
		}
		m_pState->Finish();
#endif
	}
	if (m_stThread.joinable())
	{
		m_stThread.join();
	}
	return m_pState->iCloseStatus;
}

int32_t Poll::CheckThreadAccess(const char* pOperation) const
{
#ifdef _SINGLE_THREAD_
	if (!InLoopThread())
	{
		fprintf(stderr, "single-thread access rejected operation=%s code=%d\n",
		    pOperation, OI_ERR_WRONG_THREAD);
		return OI_ERR_WRONG_THREAD;
	}
#else
	(void)pOperation;
#endif
	return OI_OK;
}

int32_t Poll::Init()
{
	// 常驻管理器在 poll 之后构造，析构时先排空它的后台请求。
	(void)BlockingWorkSessionManager::GetInstance();
	NetOs_Startup();
	m_pState->bReady = false;
#ifdef _SINGLE_THREAD_
	m_stLoopThread = std::this_thread::get_id();
	return m_pState->Prepare();
#else
	try
	{
		m_stThread = std::thread(StartThreadPollWait, this);
	}
	catch (...)
	{
		fprintf(stderr, "poll thread create failed\n");
		fflush(stderr);
		return OI_ERR_FAIL;
	}
	::OI::Thread::NativeMutexGuard stGuard(m_pState->stQueueLock);
	while (!m_pState->bStartDone)
	{
		m_pState->stStarted.Wait(m_pState->stQueueLock);
	}
	return m_pState->iStartStatus;
#endif
}

::uv_loop_s* Poll::Loop()
{
	return &m_pState->stLoop;
}

void Poll::CallOnLoop(void (*fnFunction)(void*), void* pArgumentLocal)
{
	if (InLoopThread())
	{
		if (fnFunction)
		{
			fnFunction(pArgumentLocal);
		}
		return;
	}
	PostOnLoop(fnFunction, pArgumentLocal);
}

int32_t Poll::StartListener(int32_t (*fnStart)(void*), void* pArgument)
{
	if (InLoopThread())
	{
		if (!m_pState->bReady || m_pState->bStop.load())
		{
			return OI_ERR_CANCELLED;
		}
		return fnStart(pArgument);
	}
#ifdef _SINGLE_THREAD_
	return OI_ERR_WRONG_THREAD;
#else
	struct Start
	{
		int32_t (*fnStart)(void*);
		void* pArgument;
		::OI::Thread::Mutex stMutex;
		::OI::Thread::MutexCond stDone;
		int32_t iStatus = OI_ERR_FAIL;
		bool bDone = false;
	};
	Start stStart{fnStart, pArgument, {}, {}};
	::OI::Thread::MutexGuard stLock(stStart.stMutex);
	{
		::OI::Thread::NativeMutexGuard stGuard(m_pState->stQueueLock);
		if (!m_pState->bReady || m_pState->bStop.load())
		{
			return OI_ERR_CANCELLED;
		}
		IoOp stOp{};
		stOp.iType = IO_OP_CALL;
		stOp.pArgument = &stStart;
		stOp.fnCall = [](void* pData)
		{
			auto* pStart = static_cast<Start*>(pData);
			::OI::Thread::MutexGuard stGuardLocal(pStart->stMutex);
			try
			{
				pStart->iStatus = GetInstance().m_pState->bStop.load() ? OI_ERR_CANCELLED : pStart->fnStart(pStart->pArgument);
			}
			catch (...)
			{
				::OI::Thread::ReportTaskError(std::current_exception());
			}
			pStart->bDone = true;
			pStart->stDone.Signal();
		};
		m_pState->PostLocked(stOp);
	}
	while (!stStart.bDone)
	{
		stStart.stDone.Wait(stStart.stMutex);
	}
	return stStart.iStatus;
#endif
}

void Poll::PostOnLoop(void (*fnFunction)(void*), void* pArgumentLocal)
{
	IoOp stOperation{};
	stOperation.iType = IO_OP_CALL;
	stOperation.pChannel = NULL;
	stOperation.pWatch = NULL;
	stOperation.fnCall = fnFunction;
	stOperation.pArgument = pArgumentLocal;
	m_pState->Post(stOperation);
}

void Poll::ArmTick(unsigned iRepeatMilliseconds, void (*fnFunction)())
{
	IoOp stOperation{};
	stOperation.iType = IO_OP_TICK;
	stOperation.fnTick = fnFunction;
	stOperation.iTickMilliseconds = iRepeatMilliseconds ? iRepeatMilliseconds : 50;
	m_pState->Post(stOperation);
}

void Poll::IOEventMod(Channel* pChannel)
{
	assert(InLoopThread());
	ChangeEvents(pChannel, pChannel->iEventMask, IO_EV_REG & ~pChannel->iEventMask);
}

void Poll::ChangeEvents(Channel* pChannel, int32_t iEnable, int32_t iDisable)
{
#ifndef _SINGLE_THREAD_
	if (!InLoopThread())
	{
		::OI::Thread::NativeMutexGuard stGuard(m_pState->stWatchLock);
		auto it = m_pState->mWatches.find(pChannel);
		if (it != m_pState->mWatches.end() && it->second->pChannel.load())
		{
			auto& vRequests = it->second->vRequests;
			vRequests.push_back({iEnable, iDisable});
			if (vRequests.size() == 1)
			{
				IoOp stOp{};
				stOp.iType = IO_OP_MOD;
				stOp.pWatch = it->second;
				m_pState->Post(stOp);
			}
		}
		return;
	}
#else
	if (!InLoopThread())
	{
		return;
	}
#endif
	auto it = m_pState->mWatches.find(pChannel);
	if (it != m_pState->mWatches.end())
	{
		auto& stWatch = *it->second;
#ifndef _SINGLE_THREAD_
		m_pState->ApplyPending(stWatch);
#endif
		SetEvents(stWatch, (stWatch.iMask | iEnable) & ~iDisable);
	}
}

void Poll::PauseEvents(Channel* pChannel, unsigned iMilliseconds)
{
	assert(InLoopThread());
	auto it = m_pState->mWatches.find(pChannel);
	if (it == m_pState->mWatches.end() || !pChannel->iEventMask)
	{
		return;
	}
	auto& stWatch = *it->second;
	if (!stWatch.pResumeTimer)
	{
		auto pTimer = std::make_unique<uv_timer_t>();
		if (uv_timer_init(&m_pState->stLoop, pTimer.get()) != 0)
		{
			return;
		}
		pTimer->data = &stWatch;
		stWatch.pResumeTimer = pTimer.release();
	}
	stWatch.iPausedMask = pChannel->iEventMask;
	if (uv_timer_start(stWatch.pResumeTimer, [](uv_timer_t* pHandle)
	{
		auto* pWatch = static_cast<IoWatch*>(pHandle->data);
		if (auto* pLive = pWatch->pChannel.load())
		{
			GetInstance().ChangeEvents(pLive, pWatch->iPausedMask, IO_EV_REG & ~pWatch->iPausedMask);
		}
	}, iMilliseconds, 0) != 0)
	{
		return;
	}
	ChangeEvents(pChannel, 0, IO_EV_REG);
}

void Poll::AdoptWatchTimer(Channel* pChannel, uv_timer_s* pTimer)
{
	assert(InLoopThread());
	auto& stWatch = *m_pState->mWatches.at(pChannel);
	assert(!stWatch.pTimer);
	stWatch.pTimer = pTimer;
}

int32_t Poll::IOEventRegister(Channel* pPointer, bool bAllowRead, bool bAllowWrite)
{
	struct Registration
	{
		Channel* pChannel;
		int32_t iMask;
	} stRegistration{pPointer, (bAllowRead ? IO_EV_READ : 0) | (bAllowWrite ? IO_EV_WRITE : 0)};
	return StartListener([](void* pArgument) -> int32_t
	{
		auto& stRequest = *static_cast<Registration*>(pArgument);
		return GetInstance().RegisterOnLoop(stRequest.pChannel, stRequest.iMask);
	}, &stRegistration);
}

int32_t Poll::RegisterOnLoop(Channel* pChannel, int32_t iMask)
{
	assert(InLoopThread());
	auto& stState = *m_pState;
	::OI::Thread::NativeMutexGuard stGuard(stState.stWatchLock);
	auto pWatch = std::make_shared<IoWatch>();
	auto [it, bInserted] = stState.mWatches.try_emplace(pChannel, nullptr);
	if (!bInserted)
	{
		return OI_ERR_EXIST;
	}
	pWatch->iDescriptor = pChannel->stSocket.GetSocket();
	pWatch->iMask = iMask;
	int iStatus = uv_poll_init_socket(&stState.stLoop, &pWatch->stPoll, (uv_os_sock_t)pWatch->iDescriptor);
	const bool bInitialized = iStatus == 0;
	if (bInitialized)
	{
		pWatch->stPoll.data = pWatch.get();
		if (iMask)
		{
			iStatus = uv_poll_start(&pWatch->stPoll, MaskToUv(iMask), OnPoll);
		}
	}
	if (iStatus != 0)
	{
		fprintf(stderr, "poll register %s failed: %s (%d)\n", bInitialized ? "start" : "init", uv_strerror(iStatus), iStatus);
		stState.mWatches.erase(it);
		pChannel->DetachSocket();
		const auto iDescriptor = pWatch->iDescriptor;
		if (bInitialized)
		{
			pWatch->iDescriptor = INVALID_SOCKET_ID;
			CloseWatchOnLoop(pWatch, false);
		}
		NetOs_Close(iDescriptor);
		return OI_ERR_SOCKET;
	}
	pChannel->iEventMask = iMask;
	pWatch->pChannel.store(pChannel);
	it->second = std::move(pWatch);
	return OI_OK;
}

void Poll::IOEventUnRegister(Channel* pChannel, bool bCloseDescriptor)
{
	::OI::Thread::NativeMutexGuard stGuard(m_pState->stWatchLock);
	auto it = m_pState->mWatches.find(pChannel);
	if (it == m_pState->mWatches.end())
	{
		return;
	}
#ifdef _SINGLE_THREAD_
	if (!InLoopThread())
	{
		fprintf(stderr, "single-thread registered channel unregister outside poll thread channel=%p\n", static_cast<void*>(pChannel));
		fflush(stderr);
		std::abort();
	}
#endif
	auto pWatch = it->second;
	pWatch->pChannel.store(nullptr);
#ifndef _SINGLE_THREAD_
	if (!InLoopThread())
	{
		pWatch->bCloseDescriptor = bCloseDescriptor;
		IoOp stOp{};
		stOp.iType = IO_OP_DEL;
		stOp.pWatch = pWatch;
		stOp.pChannel = pChannel;
		m_pState->Post(stOp);
		// loop 上已取走通道的回调先返回，调用方才可释放通道或复用 fd。
		while (!pWatch->bRemoved)
		{
			m_pState->stRemoved.Wait(m_pState->stWatchLock);
		}
		pChannel->DetachSocket();
		return;
	}
#endif
	pChannel->DetachSocket();
	m_pState->mWatches.erase(it);
	CloseWatchOnLoop(pWatch, bCloseDescriptor);
#ifndef _SINGLE_THREAD_
	pWatch->bRemoved = true;
	m_pState->stRemoved.Broadcast();
#endif
}

bool Poll::InPollCallback() const
{
#ifdef _SINGLE_THREAD_
	return InLoopThread() && m_pState->iPollDepth != 0;
#else
	return InLoopThread();
#endif
}

void Poll::PollWait(int iWaitMilliseconds)
{
#ifdef _SINGLE_THREAD_
	struct Guard
	{
		unsigned& iDepth;
		explicit Guard(unsigned& iDepth)
		    : iDepth(iDepth)
		{
			++iDepth;
		}
		~Guard()
		{
			--iDepth;
		}
	} stGuard(m_pState->iPollDepth);
	uv_run(&m_pState->stLoop, iWaitMilliseconds == 0 ? UV_RUN_NOWAIT : UV_RUN_ONCE);
#else
	(void)iWaitMilliseconds;
#endif
}
}
}
