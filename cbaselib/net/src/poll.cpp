#ifdef _WIN32
#include <winsock2.h>
#include <windows.h>
#endif
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <stdlib.h>
#include <deque>
#include <mutex>
#ifndef _WIN32
#include <chrono>
#include <thread>
#endif
#include <uv.h>
#include "poll.h"
#include "channel.h"
#include "task.h"
#include "threadpool.h"
#include "header.h"
#include "net_os.h"

static const void* POLL_PENDING = (void*)1;
static const void* POLL_DROPPED = (void*)2;

struct IoWatch
{
	uv_poll_t poll;
	net_socket_t fd;
	int mask;
};

enum IoOpType
{
	IO_OP_ADD,
	IO_OP_MOD,
	IO_OP_DEL,
	IO_OP_TICK,
};

struct IoOp
{
	IoOpType type;
	Channel* channel;
	IoWatch* watch;
	net_socket_t fd;
	int mask;
};

static uv_loop_t g_loop;
static uv_async_t g_async;
static uv_timer_t g_tick_timer;
static std::mutex g_op_lock;
static std::deque<IoOp> g_ops;
static bool g_loop_ready = false;
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

class PollInnerTask
{
	Task* _p_task;
	bool  _once;
public:
	PollInnerTask(Task* pTask, bool Once) :_p_task(pTask), _once(Once) {}
	bool Exec()
	{
		assert(_p_task);
		_p_task->Exec();
		return _once;
	}
	~PollInnerTask()
	{
		SAFE_DELETE(_p_task);
	}
};

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
	if (!op.watch)
	{
		return;
	}
	if (op.type == IO_OP_MOD)
	{
		op.watch->mask = op.mask;
		uv_poll_start(&op.watch->poll, MaskToUv(op.mask), OnPoll);
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
			std::lock_guard<std::mutex> guard(g_op_lock);
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
	std::lock_guard<std::mutex> guard(g_op_lock);
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
		std::lock_guard<std::mutex> guard(g_op_lock);
		g_loop_ready = true;
	}
	DrainOps();
	StartTickOnLoop();
}

Poll Poll::inst;

Poll::~Poll()
{
}

void* Poll::StartThreadPollWait(void*)
{
	ThreadPool::InitIgnoreSig();
	NetOs_Startup();
	PrepareLoop();
	for (;;)
	{
		Poll::GetInstance()->BeforePoll();
		uv_run(&g_loop, UV_RUN_ONCE);
		Poll::GetInstance()->AfterPoll();
	}
	return NULL;
}

bool Poll::Init()
{
	NetOs_Startup();
	g_loop_ready = false;
#ifdef _SINGLE_THREAD_
	PrepareLoop();
	return true;
#else
	if (!ThreadPool::CreateSpecThread(_loop_thread, StartThreadPollWait, NULL))
	{
		return false;
	}
	for (;;)
	{
		bool ready = false;
		{
			std::lock_guard<std::mutex> guard(g_op_lock);
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
	p->_poll_token = (void*)POLL_PENDING;
	IoOp op;
	op.type = IO_OP_ADD;
	op.channel = p;
	op.watch = NULL;
	op.fd = p->_socket.GetSocket();
	op.mask = new_mask;
	PostOp(op);
}

void Poll::IOEventUnRegister(Channel* p)
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
	IoOp op;
	op.type = IO_OP_DEL;
	op.channel = p;
	op.watch = watch;
	op.fd = watch->fd;
	op.mask = 0;
	PostOp(op);
}

void Poll::AddPollTask(Task* pTask, PollTaskPosEnum pos, bool once)
{
	if (pos == PollTaskPosEnum::POLL_AFTER_TASK)
	{
		_before_tasks.emplace_back(new PollInnerTask(pTask, once));
	}
	else
	{
		_after_tasks.emplace_back(new PollInnerTask(pTask, once));
	}
}

void Poll::BeforePoll()
{
	for (PollTaskList::iterator it = _before_tasks.begin(); it != _before_tasks.end();)
	{
		bool once = (*it)->Exec();
		if (once)
		{
			it = _before_tasks.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void Poll::AfterPoll()
{
	for (PollTaskList::iterator it = _after_tasks.begin(); it != _after_tasks.end();)
	{
		bool once = (*it)->Exec();
		if (once)
		{
			it = _after_tasks.erase(it);
		}
		else
		{
			++it;
		}
	}
}

void Poll::PollWait(int)
{
#ifdef _SINGLE_THREAD_
	BeforePoll();
	uv_run(&g_loop, UV_RUN_ONCE);
	AfterPoll();
#endif
}
