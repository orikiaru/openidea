#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
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
namespace OI { namespace Net {

static const void* POLL_PENDING = (void*)1;
static const void* POLL_DROPPED = (void*)2;

struct IoWatch
{
	uv_poll_t poll;
	net_socket_t fd;
	int32_t mask;
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
	IoOpType type;
	Channel* channel;
	IoWatch* watch;
	net_socket_t fd;
	int32_t mask;
	void (*call)(void*);
	void* arg;
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

static int MaskToUv(int mask)
{
	int events = 0;
	if (mask & IO_EV_READ)
	{
		events |= UV_READABLE;
	}
	if (mask & IO_EV_WRITE)
	{
		events |= UV_WRITABLE;
	}
	if (events == 0)
	{
		events = UV_READABLE;
	}
	return events;
}

static void OnPoll(uv_poll_t* handle, int status, int events)
{
	Channel* channel = (Channel*)handle->data;
	if (!channel)
	{
		return;
	}
	if (status < 0 || (events & UV_READABLE) || (events & UV_DISCONNECT))
	{
		channel->DataIn();
	}
	if (events & UV_WRITABLE)
	{
		channel->DataOut();
	}
	channel->TryClose();
}

static void CloseWatch(uv_handle_t* handle)
{
	IoWatch* watch = (IoWatch*)handle;
	NetOs_Close(watch->fd);
	delete watch;
}

static void ApplyOp(const IoOp& op)
{
	if (op.type == IO_OP_ADD)
	{
		if (op.channel->_poll_token == POLL_DROPPED)
		{
			NetOs_Close(op.fd);
			op.channel->_poll_token = NULL;
			return;
		}
		IoWatch* watch = new IoWatch();
		watch->fd = op.fd;
		watch->mask = op.channel->_event_mask;
		int init_rc = uv_poll_init_socket(&g_loop, &watch->poll, (uv_os_sock_t)op.fd);
		if (init_rc != 0)
		{
			// 套接字还在 channel 上。先摘掉再关，析构里就不会再关一次。
			op.channel->DetachSocket();
			NetOs_Close(op.fd);
			delete watch;
			op.channel->_poll_token = NULL;
			return;
		}
		watch->poll.data = op.channel;
		uv_poll_start(&watch->poll, MaskToUv(watch->mask), OnPoll);
		op.channel->_poll_token = watch;
		return;
	}
	if (op.type == IO_OP_TICK)
	{
		StartTickOnLoop();
		return;
	}
	if (op.type == IO_OP_CALL)
	{
		if (op.call)
		{
			op.call(op.arg);
		}
		return;
	}
	if (!op.watch)
	{
		return;
	}
	if (op.type == IO_OP_MOD)
	{
		Channel* channel = (Channel*)op.watch->poll.data;
		if (channel == NULL)
		{
			return;
		}
		// 排队期间 poll 线程可能已经直接改过。按 channel 当前的掩码来，旧操作不会盖掉新状态。
		int mask = channel->_event_mask;
		if (op.watch->mask != mask)
		{
			op.watch->mask = mask;
			if (mask == 0)
			{
				uv_poll_stop(&op.watch->poll);
			}
			else
			{
				uv_poll_start(&op.watch->poll, MaskToUv(mask), OnPoll);
			}
		}
		return;
	}
	uv_poll_stop(&op.watch->poll);
	uv_close((uv_handle_t*)&op.watch->poll, CloseWatch);
}

static void DrainOps()
{
	for (;;)
	{
		IoOp op;
		{
			::OI::Thread::MutexGuard guard(g_op_lock);
			if (g_ops.empty())
			{
				return;
			}
			op = g_ops.front();
			g_ops.pop_front();
		}
		ApplyOp(op);
	}
}

static void OnAsync(uv_async_t*)
{
	DrainOps();
}

static void PostOp(const IoOp& op)
{
	::OI::Thread::MutexGuard guard(g_op_lock);
	g_ops.push_back(op);
	if (g_loop_ready)
	{
		uv_async_send(&g_async);
	}
}

static void PrepareLoop()
{
	uv_loop_init(&g_loop);
	uv_async_init(&g_loop, &g_async, OnAsync);
	{
		::OI::Thread::MutexGuard guard(g_op_lock);
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
	GetInstance()._loop_thread = std::this_thread::get_id();
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
	if (_thread.joinable())
	{
		_thread.join();
	}
}

bool Poll::Init()
{
	NetOs_Startup();
	g_loop_ready = false;
#ifdef _SINGLE_THREAD_
	_loop_thread = std::this_thread::get_id();
	PrepareLoop();
	return true;
#else
	try
	{
		_thread = std::thread(StartThreadPollWait, (void*)NULL);
	}
	catch (...)
	{
		return false;
	}
	for (;;)
	{
		bool ready = false;
		{
			::OI::Thread::MutexGuard guard(g_op_lock);
			ready = g_loop_ready;
		}
		if (ready)
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

void Poll::CallOnLoop(void (*fn)(void*), void* arg)
{
	if (InLoopThread())
	{
		if (fn)
		{
			fn(arg);
		}
		return;
	}
	IoOp op;
	op.type = IO_OP_CALL;
	op.channel = NULL;
	op.watch = NULL;
	op.fd = 0;
	op.mask = 0;
	op.call = fn;
	op.arg = arg;
	PostOp(op);
}

void Poll::ArmTick(unsigned repeat_ms, void (*fn)())
{
	g_tick_ms = repeat_ms ? repeat_ms : 50;
	g_tick_fn = fn;
	if (!g_loop_ready)
	{
		return;
	}
	IoOp op;
	op.type = IO_OP_TICK;
	op.channel = NULL;
	op.watch = NULL;
	op.fd = 0;
	op.mask = 0;
	PostOp(op);
}

void Poll::IOEventMod(Channel* p, int new_mask)
{
	p->_event_mask = new_mask;
	IoWatch* watch = (IoWatch*)p->_poll_token;
	if (!watch || watch == POLL_PENDING || watch == POLL_DROPPED)
	{
		return;
	}
	if (InLoopThread())
	{
		if (watch->poll.data == NULL)
		{
			return;
		}
		watch->mask = new_mask;
		if (new_mask == 0)
		{
			uv_poll_stop(&watch->poll);
			return;
		}
		uv_poll_start(&watch->poll, MaskToUv(new_mask), OnPoll);
		return;
	}
	IoOp op;
	op.type = IO_OP_MOD;
	op.channel = p;
	op.watch = watch;
	op.fd = watch->fd;
	op.mask = new_mask;
	PostOp(op);
}

void Poll::IOEventRegister(Channel* p, bool allow_read, bool allow_write)
{
	int new_mask = 0;
	if (allow_read)
	{
		new_mask |= IO_EV_READ;
	}
	if (allow_write)
	{
		new_mask |= IO_EV_WRITE;
	}
	p->_event_mask = new_mask;
	IoOp op;
	op.type = IO_OP_ADD;
	op.channel = p;
	op.watch = NULL;
	op.fd = p->_socket.GetSocket();
	op.mask = new_mask;
	// 已经在 poll 线程就当场挂上。排队的话，同一次调用里接着改掩码会赶不上这次 ADD。
	if (InLoopThread() && g_loop_ready)
	{
		p->_poll_token = NULL;
		ApplyOp(op);
		return;
	}
	p->_poll_token = (void*)POLL_PENDING;
	PostOp(op);
}

void Poll::IOEventUnRegister(Channel* p, bool close_fd)
{
	void* token = p->_poll_token;
	if (token == NULL || token == POLL_DROPPED)
	{
		return;
	}
	if (token == POLL_PENDING)
	{
		p->_poll_token = (void*)POLL_DROPPED;
		p->_socket.Detach();
		return;
	}
	IoWatch* watch = (IoWatch*)token;
	p->_poll_token = NULL;
	p->_socket.Detach();
	if (!close_fd)
	{
		watch->fd = INVALID_SOCKET_ID;
	}
	// 就在 poll 线程上。epoll 是水平触发，把 stop 推迟到下一轮的话，
	// 回调返回后同一条连接还会再进一次，channel 已经 delete 了。
	if (std::this_thread::get_id() == _loop_thread)
	{
		watch->poll.data = NULL;
		uv_poll_stop(&watch->poll);
	}
	IoOp op;
	op.type = IO_OP_DEL;
	op.channel = p;
	op.watch = watch;
	op.fd = watch->fd;
	op.mask = 0;
	PostOp(op);
}

void Poll::PollWait(int wait_ms)
{
#ifdef _SINGLE_THREAD_
	uv_run(&g_loop, wait_ms == 0 ? UV_RUN_NOWAIT : UV_RUN_ONCE);
#else
	(void)wait_ms;
#endif
}
}}
