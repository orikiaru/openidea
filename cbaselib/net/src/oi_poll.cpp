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
#include "oi_lock.h"
#ifndef _WIN32
#include <chrono>
#include <thread>
#endif
#include <uv.h>
#include "oi_poll.h"
#include "oi_channel.h"
#include "oi_thread.h"
#include "oi_header.h"
#include "oi_net_os.h"
namespace OI
{
namespace Net
{

static const void* POLL_PENDING = (void*)1;
static const void* POLL_DROPPED = (void*)2;

struct IoWatch
{
	uv_poll_t stPoll;
	net_socket_t iDescriptor;
	int32_t iMask;
};

enum IoOpType
{
	IO_OP_ADD,
	IO_OP_MOD,
	IO_OP_DEL,
	IO_OP_TICK,
	IO_OP_CALL,
};

struct IoOp
{
	IoOpType iType;
	Channel* pChannel;
	IoWatch* pWatch;
	net_socket_t iDescriptor;
	int32_t iMask;
	void (*fnCall)(void*);
	void* pArgument;
};

static uv_loop_t g_loop;
static uv_async_t g_async;
static uv_timer_t g_tick_timer;
static ::OI::Thread::Mutex g_op_lock;
static std::deque<IoOp> g_ops;
static bool g_loop_ready = false;
static std::atomic<bool> g_poll_stop(false);
static bool g_tick_armed = false;
static unsigned g_tick_ms = 50;
static void (*g_tick_fn)() = NULL;

static void OnUvTick(uv_timer_t*)
{
	if (g_tick_fn)
	{
		g_tick_fn();
	}
}

static void StartTickOnLoop()
{
	if (g_tick_armed || g_tick_fn == NULL || g_tick_ms == 0)
	{
		return;
	}
	uv_timer_init(&g_loop, &g_tick_timer);
	uv_timer_start(&g_tick_timer, OnUvTick, g_tick_ms, g_tick_ms);
	g_tick_armed = true;
}

static int MaskToUv(int iMask)
{
	int iEvents = 0;
	if (iMask & IO_EV_READ)
	{
		iEvents |= UV_READABLE;
	}
	if (iMask & IO_EV_WRITE)
	{
		iEvents |= UV_WRITABLE;
	}
	if (iEvents == 0)
	{
		iEvents = UV_READABLE;
	}
	return iEvents;
}

static void OnPoll(uv_poll_t* pHandle, int iStatus, int iEvents)
{
	Channel* pChannelLocal = (Channel*)pHandle->data;
	if (!pChannelLocal)
	{
		return;
	}
	if (iStatus < 0 || (iEvents & UV_READABLE) || (iEvents & UV_DISCONNECT))
	{
		pChannelLocal->DataIn();
	}
	if (iEvents & UV_WRITABLE)
	{
		pChannelLocal->DataOut();
	}
	pChannelLocal->TryClose();
}

static void CloseWatch(uv_handle_t* pHandle)
{
	IoWatch* pWatchLocal = (IoWatch*)pHandle;
	NetOs_Close(pWatchLocal->iDescriptor);
	delete pWatchLocal;
}

static void ApplyOp(const IoOp& stOperation)
{
	if (stOperation.iType == IO_OP_ADD)
	{
		if (stOperation.pChannel->pPollToken == POLL_DROPPED)
		{
			NetOs_Close(stOperation.iDescriptor);
			stOperation.pChannel->pPollToken = NULL;
			return;
		}
		IoWatch* pWatchLocal = new IoWatch();
		pWatchLocal->iDescriptor = stOperation.iDescriptor;
		pWatchLocal->iMask = stOperation.pChannel->iEventMask;
		int iInitReturnCode = uv_poll_init_socket(&g_loop, &pWatchLocal->stPoll, (uv_os_sock_t)stOperation.iDescriptor);
		if (iInitReturnCode != 0)
		{
			// 套接字还在 channel 上。先摘掉再关，析构里就不会再关一次。
			stOperation.pChannel->DetachSocket();
			NetOs_Close(stOperation.iDescriptor);
			delete pWatchLocal;
			stOperation.pChannel->pPollToken = NULL;
			return;
		}
		pWatchLocal->stPoll.data = stOperation.pChannel;
		uv_poll_start(&pWatchLocal->stPoll, MaskToUv(pWatchLocal->iMask), OnPoll);
		stOperation.pChannel->pPollToken = pWatchLocal;
		return;
	}
	if (stOperation.iType == IO_OP_TICK)
	{
		StartTickOnLoop();
		return;
	}
	if (stOperation.iType == IO_OP_CALL)
	{
		if (stOperation.fnCall)
		{
			stOperation.fnCall(stOperation.pArgument);
		}
		return;
	}
	if (!stOperation.pWatch)
	{
		return;
	}
	if (stOperation.iType == IO_OP_MOD)
	{
		Channel* pChannelLocal = (Channel*)stOperation.pWatch->stPoll.data;
		if (pChannelLocal == NULL)
		{
			return;
		}
		// 排队期间 poll 线程可能已经直接改过。按 channel 当前的掩码来，旧操作不会盖掉新状态。
		int iMaskLocal = pChannelLocal->iEventMask;
		if (stOperation.pWatch->iMask != iMaskLocal)
		{
			stOperation.pWatch->iMask = iMaskLocal;
			if (iMaskLocal == 0)
			{
				uv_poll_stop(&stOperation.pWatch->stPoll);
			}
			else
			{
				uv_poll_start(&stOperation.pWatch->stPoll, MaskToUv(iMaskLocal), OnPoll);
			}
		}
		return;
	}
	uv_poll_stop(&stOperation.pWatch->stPoll);
	uv_close((uv_handle_t*)&stOperation.pWatch->stPoll, CloseWatch);
}

static void DrainOps()
{
	size_t iRemaining;
	{
		::OI::Thread::MutexGuard stGuard(g_op_lock);
		iRemaining = g_ops.size();
	}
	while (iRemaining-- > 0)
	{
		IoOp stOperation;
		{
			::OI::Thread::MutexGuard stGuard(g_op_lock);
			if (g_ops.empty())
			{
				return;
			}
			stOperation = g_ops.front();
			g_ops.pop_front();
		}
		ApplyOp(stOperation);
	}
}

static void OnAsync(uv_async_t*)
{
	DrainOps();
}

static const int g_quit_signums[] = {
    SIGINT,
    SIGTERM,
#ifdef SIGQUIT
    SIGQUIT,
#endif
};
static uv_signal_t g_quit_signals[sizeof(g_quit_signums) / sizeof(g_quit_signums[0])];

// libuv 在信号处理函数里只写管道，这里已经回到 loop，可以正常加锁、通知。
static void OnQuitSignal(uv_signal_t*, int)
{
	::OI::Thread::ThreadPool::GetInstance().RequestStop(0);
}

static void StartQuitSignals()
{
	for (size_t iIndexLocal = 0; iIndexLocal < sizeof(g_quit_signums) / sizeof(g_quit_signums[0]); ++iIndexLocal)
	{
		uv_signal_init(&g_loop, &g_quit_signals[iIndexLocal]);
		uv_signal_start(&g_quit_signals[iIndexLocal], OnQuitSignal, g_quit_signums[iIndexLocal]);
	}
}

static void PostOp(const IoOp& stOperation)
{
	::OI::Thread::MutexGuard stGuard(g_op_lock);
	g_ops.push_back(stOperation);
	if (g_loop_ready)
	{
		uv_async_send(&g_async);
	}
}

static void PrepareLoop()
{
	uv_loop_init(&g_loop);
	uv_async_init(&g_loop, &g_async, OnAsync);
	StartQuitSignals();
	{
		::OI::Thread::MutexGuard stGuard(g_op_lock);
		g_loop_ready = true;
	}
	DrainOps();
	StartTickOnLoop();
}

Poll::~Poll()
{
	// 启动失败提前 return 时，poll 线程还活着。析构不 join 会 std::terminate，把真正的错误盖掉。
	StopAndJoin();
}

void* Poll::StartThreadPollWait(void*)
{
	GetInstance().m_stLoopThread = std::this_thread::get_id();
	::OI::Thread::ThreadPool::InitIgnoreSig();
	NetOs_Startup();
	PrepareLoop();
	while (!g_poll_stop.load())
	{
		uv_run(&g_loop, UV_RUN_ONCE);
	}
	return NULL;
}

void Poll::StopAndJoin()
{
	g_poll_stop.store(true);
	if (g_loop_ready)
	{
		uv_async_send(&g_async);
	}
	if (m_stThread.joinable())
	{
		m_stThread.join();
	}
}

bool Poll::Init()
{
	NetOs_Startup();
	g_loop_ready = false;
#ifdef _SINGLE_THREAD_
	m_stLoopThread = std::this_thread::get_id();
	PrepareLoop();
	return true;
#else
	try
	{
		m_stThread = std::thread(StartThreadPollWait, (void*)NULL);
	}
	catch (...)
	{
		fprintf(stderr, "poll thread create failed\n");
		fflush(stderr);
		return false;
	}
	for (;;)
	{
		bool bReadyLocal = false;
		{
			::OI::Thread::MutexGuard stGuard(g_op_lock);
			bReadyLocal = g_loop_ready;
		}
		if (bReadyLocal)
		{
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	return true;
#endif
}

::uv_loop_s* Poll::Loop()
{
	return &g_loop;
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

void Poll::PostOnLoop(void (*fnFunction)(void*), void* pArgumentLocal)
{
	IoOp stOperation;
	stOperation.iType = IO_OP_CALL;
	stOperation.pChannel = NULL;
	stOperation.pWatch = NULL;
	stOperation.iDescriptor = 0;
	stOperation.iMask = 0;
	stOperation.fnCall = fnFunction;
	stOperation.pArgument = pArgumentLocal;
	PostOp(stOperation);
}

void Poll::ArmTick(unsigned iRepeatMilliseconds, void (*fnFunction)())
{
	g_tick_ms = iRepeatMilliseconds ? iRepeatMilliseconds : 50;
	g_tick_fn = fnFunction;
	if (!g_loop_ready)
	{
		return;
	}
	IoOp stOperation;
	stOperation.iType = IO_OP_TICK;
	stOperation.pChannel = NULL;
	stOperation.pWatch = NULL;
	stOperation.iDescriptor = 0;
	stOperation.iMask = 0;
	PostOp(stOperation);
}

void Poll::IOEventMod(Channel* pPointer, int iNewMask)
{
	pPointer->iEventMask = iNewMask;
	IoWatch* pWatchLocal = (IoWatch*)pPointer->pPollToken;
	if (!pWatchLocal || pWatchLocal == POLL_PENDING || pWatchLocal == POLL_DROPPED)
	{
		return;
	}
	if (InLoopThread())
	{
		if (pWatchLocal->stPoll.data == NULL)
		{
			return;
		}
		pWatchLocal->iMask = iNewMask;
		if (iNewMask == 0)
		{
			uv_poll_stop(&pWatchLocal->stPoll);
			return;
		}
		uv_poll_start(&pWatchLocal->stPoll, MaskToUv(iNewMask), OnPoll);
		return;
	}
	IoOp stOperation;
	stOperation.iType = IO_OP_MOD;
	stOperation.pChannel = pPointer;
	stOperation.pWatch = pWatchLocal;
	stOperation.iDescriptor = pWatchLocal->iDescriptor;
	stOperation.iMask = iNewMask;
	PostOp(stOperation);
}

struct PausedChannel
{
	uv_timer_t stTimer;
	Channel* pChannel;
	int32_t iMask;
};

static void FreePaused(uv_handle_t* pHandle)
{
	delete (PausedChannel*)pHandle->data;
}

static void OnPauseEnd(uv_timer_t* pTimer)
{
	PausedChannel* pPaused = (PausedChannel*)pTimer->data;
	Poll::GetInstance().IOEventMod(pPaused->pChannel, pPaused->iMask);
	uv_close((uv_handle_t*)pTimer, FreePaused);
}

void Poll::PauseEvents(Channel* pPointer, unsigned iMilliseconds)
{
	assert(InLoopThread());
	// 已经停着的不再叠一次，否则恢复时拿到的是 0。
	if (pPointer->iEventMask == 0)
	{
		return;
	}
	PausedChannel* pPaused = new PausedChannel();
	pPaused->pChannel = pPointer;
	pPaused->iMask = pPointer->iEventMask;
	pPaused->stTimer.data = pPaused;
	IOEventMod(pPointer, 0);
	uv_timer_init(&g_loop, &pPaused->stTimer);
	uv_timer_start(&pPaused->stTimer, OnPauseEnd, iMilliseconds, 0);
}

void Poll::IOEventRegister(Channel* pPointer, bool bAllowRead, bool bAllowWrite)
{
	int iNewMask = 0;
	if (bAllowRead)
	{
		iNewMask |= IO_EV_READ;
	}
	if (bAllowWrite)
	{
		iNewMask |= IO_EV_WRITE;
	}
	pPointer->iEventMask = iNewMask;
	IoOp stOperation;
	stOperation.iType = IO_OP_ADD;
	stOperation.pChannel = pPointer;
	stOperation.pWatch = NULL;
	stOperation.iDescriptor = pPointer->stSocket.GetSocket();
	stOperation.iMask = iNewMask;
	// 已经在 poll 线程就当场挂上。排队的话，同一次调用里接着改掩码会赶不上这次 ADD。
	if (InLoopThread() && g_loop_ready)
	{
		pPointer->pPollToken = NULL;
		ApplyOp(stOperation);
		return;
	}
	pPointer->pPollToken = (void*)POLL_PENDING;
	PostOp(stOperation);
}

void Poll::IOEventUnRegister(Channel* pPointer, bool bCloseDescriptor)
{
	void* pToken = pPointer->pPollToken;
	if (pToken == NULL || pToken == POLL_DROPPED)
	{
		return;
	}
	if (pToken == POLL_PENDING)
	{
		pPointer->pPollToken = (void*)POLL_DROPPED;
		pPointer->stSocket.Detach();
		return;
	}
	IoWatch* pWatchLocal = (IoWatch*)pToken;
	pPointer->pPollToken = NULL;
	pPointer->stSocket.Detach();
	if (!bCloseDescriptor)
	{
		pWatchLocal->iDescriptor = INVALID_SOCKET_ID;
	}
	// 就在 poll 线程上。epoll 是水平触发，把 stop 推迟到下一轮的话，
	// 回调返回后同一条连接还会再进一次，channel 已经 delete 了。
	if (std::this_thread::get_id() == m_stLoopThread)
	{
		pWatchLocal->stPoll.data = NULL;
		uv_poll_stop(&pWatchLocal->stPoll);
	}
	IoOp stOperation;
	stOperation.iType = IO_OP_DEL;
	stOperation.pChannel = pPointer;
	stOperation.pWatch = pWatchLocal;
	stOperation.iDescriptor = pWatchLocal->iDescriptor;
	stOperation.iMask = 0;
	PostOp(stOperation);
}

namespace
{
[[maybe_unused]] thread_local unsigned g_poll_depth = 0;
}

bool Poll::InPollCallback() const
{
#ifdef _SINGLE_THREAD_
	return g_poll_depth != 0;
#else
	return InLoopThread();
#endif
}

void Poll::PollWait(int iWaitMilliseconds)
{
#ifdef _SINGLE_THREAD_
	struct Guard
	{
		Guard()
		{
			++g_poll_depth;
		}
		~Guard()
		{
			--g_poll_depth;
		}
	} stGuard;
	uv_run(&g_loop, iWaitMilliseconds == 0 ? UV_RUN_NOWAIT : UV_RUN_ONCE);
#else
	(void)iWaitMilliseconds;
#endif
}
}
}
