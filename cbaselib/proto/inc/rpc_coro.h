#pragma once
#include "bytes_stream.h"
#include "header.h"
#include "parser.h"
#include "rpc.h"
#include "session_manager.h"
#include "spin_lock.h"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <map>
#include <utility>
#include <vector>

namespace coro
{
class RpcError : public std::exception
{
	int _code;
public:
	explicit RpcError(int code) : _code(code) {}
	int code() const { return _code; }
	const char* what() const noexcept override { return "RpcError"; }
};

class Hub
{
public:
	static uint64_t Add(std::function<void(int, const void*)> done);
	static void Bind(uint64_t id, std::function<void(int, const void*)> done);
	static uint64_t LastId();
	static int Pending();
	template<typename Res>
	static void Complete(uint64_t id, int code, const Res* value)
	{
		CompleteRaw(id, code, value);
	}
	static bool CompleteRaw(uint64_t id, int code, const void* value);
};

void ArmRpcTimeout(uint64_t rid, int seconds);

// 协程 token。StartTask 生成并传进协程，Wake 用同一个值。
typedef uint64_t Token;
typedef Token TaskUid;

TaskUid AllocTaskUid();

// 挂起的协程放回运行队列。单线程进本线程队列，多线程进工人队列。RPC 回包和定时器都走这里。
void Schedule(std::coroutine_handle<> handle);
void NotePollWake();

// 根协程跑完后不能在 final_suspend 里销毁自己。登记下来，等 resume 返回再释放。
void DetachFinished(std::coroutine_handle<> handle);
void DrainDetached();

// 单线程：队列空了再进 poll。
class StScheduler
{
public:
	static void Post(std::coroutine_handle<> handle);
	static bool Poll();
};

// 下一轮再跑，或者过 ms 毫秒再跑。到点就是 Schedule。
struct SleepMs
{
	int ms;
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) const noexcept;
	void await_resume() const noexcept {}
};

inline SleepMs yield(int ms)
{
	return SleepMs{ms};
}

struct YieldToPool
{
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) const noexcept;
	void await_resume() const noexcept {}
};

// 停住当前协程。还没停到这里就 Wake，会先记下，这次 Park 直接过去。
struct Park
{
	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> h) const
	{
		return ParkAt(h.promise().uid, h);
	}
	void await_resume() const noexcept {}
};

bool ParkAt(TaskUid uid, std::coroutine_handle<> handle);
bool Wake(TaskUid uid);

struct ThisUid
{
	TaskUid value;
	ThisUid() : value(0) {}
	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> h)
	{
		value = h.promise().uid;
		return false;
	}
	TaskUid await_resume() const noexcept { return value; }
};

// 子任务跑完时：调用方已经挂起，就把调用方 Schedule 回去；还没挂起，就只做个标记。
struct TaskFinalSuspend
{
	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	void await_suspend(std::coroutine_handle<Promise> h) const noexcept
	{
		auto& promise = h.promise();
		if (promise.ready.exchange(true, std::memory_order_acq_rel))
		{
			if (promise.continuation)
			{
				Schedule(promise.continuation);
			}
			return;
		}
		if (promise.detached)
		{
			DetachFinished(h);
		}
	}
	void await_resume() const noexcept {}
};

struct TaskState
{
	Token uid;
	std::coroutine_handle<> continuation;
	bool detached;
	std::atomic<bool> ready;
	TaskState() : uid(0), continuation(), detached(false), ready(false) {}
	std::suspend_always initial_suspend() noexcept { return {}; }
	TaskFinalSuspend final_suspend() noexcept { return {}; }
	void return_void() {}
	void unhandled_exception() { std::terminate(); }
	SleepMs yield_value(int ms) noexcept { return SleepMs{ms}; }
	template<typename T>
	YieldToPool yield_value(T&&) noexcept { return {}; }
};

// 一条任务。Start 丢进队列；co_await 就是调用它，跑完回到调用方。
struct Task
{
	struct promise_type : TaskState
	{
		Task get_return_object()
		{
			return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
		}
	};
	std::coroutine_handle<promise_type> handle;
	explicit Task(std::coroutine_handle<promise_type> h) : handle(h) {}
	Task(Task&& other) noexcept : handle(other.handle) { other.handle = {}; }
	Task(const Task&) = delete;
	~Task() { if (handle) handle.destroy(); }
	Token Start()
	{
		if (handle.promise().uid == 0)
		{
			handle.promise().uid = AllocTaskUid();
		}
		Token token = handle.promise().uid;
		// 句柄交给调度器。调用方手里的临时 Task 析构时不能把正在跑的帧释放掉。
		handle.promise().detached = true;
		std::coroutine_handle<> h = handle;
		handle = nullptr;
		Schedule(h);
		return token;
	}
	bool await_ready() const noexcept { return !handle || handle.done(); }
	// 子协程当场跑完就返回 false，调用方不挂起。
	// 子协程自己停住（等回包或定时）时，把调用方记在 continuation 上，结束再 Schedule。
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> parent) noexcept
	{
		if (!handle)
		{
			return false;
		}
		handle.promise().continuation = parent;
		handle.resume();
		return !handle.promise().ready.exchange(true, std::memory_order_acq_rel);
	}
	void await_resume() const noexcept {}
};

template<typename F>
Token StartTask(F&& fn)
{
	Token token = AllocTaskUid();
	auto task = fn(token);
	task.handle.promise().uid = token;
	task.Start();
	return token;
}

// 下面三个都是同一件事：先挂上回调，回调里 Schedule 当前协程。
template<typename Req, typename Res>
struct RpcAwaiter
{
	const Req* req;
	Res* res;
	int result;
	uint64_t id;
	std::coroutine_handle<> handle;

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> h)
	{
		handle = h;
		result = -1;
		id = Hub::Add([this](int code, const void* value) {
			result = code;
			if (code == 0 && value && res)
			{
				*res = *static_cast<const Res*>(value);
			}
			Schedule(handle);
		});
	}
	int await_resume() const noexcept { return result; }
};

template<typename Res>
struct RidAwaiter
{
	Res* res;
	uint64_t rid;
	int timeout_sec;
	int result;
	std::coroutine_handle<> handle;

	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> h)
	{
		handle = h;
		result = -1;
		uint64_t key = rid & ~((uint64_t)RPC_REQ_MASK);
		Hub::Bind(key, [this](int code, const void* value) {
			result = code;
			if (code == 0 && value && res)
			{
				BytesStream* in = (BytesStream*)value;
				(*in) >> *res;
			}
			Schedule(handle);
		});
		if (timeout_sec > 0)
		{
			ArmRpcTimeout(key, timeout_sec);
		}
	}
	int await_resume() const noexcept { return result; }
};

template<typename Res>
RidAwaiter<Res> WaitRid(uint64_t rid, Res& res, int timeout_sec = 25)
{
	RidAwaiter<Res> awaiter;
	awaiter.res = &res;
	awaiter.rid = rid;
	awaiter.timeout_sec = timeout_sec;
	awaiter.result = -1;
	awaiter.handle = {};
	return awaiter;
}

template<typename Req, typename Res>
RpcAwaiter<Req, Res> AwaitRpc(const Req& req, Res& res)
{
	RpcAwaiter<Req, Res> awaiter;
	awaiter.req = &req;
	awaiter.res = &res;
	awaiter.result = -1;
	awaiter.id = 0;
	awaiter.handle = {};
	return awaiter;
}
}

template<typename T>
struct RpcProtoId;

void BindRpcHandler(proto_type_t id, std::function<coro::Task(coro::Token, SessionManager*, sid_t, BytesStream)> handle);

// 业务任务外面这一层：解包，co_await 业务任务，再按接受顺序把回包写出去。
template<typename Req, typename Res, typename F, typename Lane>
coro::Task RunHandledRpc(F fn, Lane* lane, proto_type_t res_id, uint64_t seq, coro::Token token, SessionManager* mgr, sid_t sid, BytesStream body)
{
	Req req;
	Res res;
	try
	{
		req.deserialize(body);
		auto child = fn(token, mgr, sid, req, res);
		if (child.handle)
		{
			child.handle.promise().uid = token;
		}
		co_await child;
		BytesStream out;
		res.serialize(out);
		BytesStream frame = EncodePbFrame(res_id, out);
		{
			MutexGuard lock(lane->mu);
			lane->ready[seq] = std::make_pair(sid, frame.GetMutableBuff());
			while (lane->ready.find(lane->next) != lane->ready.end())
			{
				std::pair<sid_t, Bytes> item = lane->ready[lane->next];
				lane->ready.erase(lane->next);
				lane->next++;
				mgr->SendBytes(item.first, item.second);
			}
		}
	}
	catch (ParserException&)
	{
		MutexGuard lock(lane->mu);
		lane->ready[seq] = std::make_pair(sid, Bytes());
		while (lane->ready.find(lane->next) != lane->ready.end())
		{
			std::pair<sid_t, Bytes> item = lane->ready[lane->next];
			lane->ready.erase(lane->next);
			lane->next++;
			if (item.second.Size() > 0)
			{
				mgr->SendBytes(item.first, item.second);
			}
		}
	}
	co_return;
}

template<typename Req, typename Res, typename F>
void HandleRpc(F fn)
{
	const proto_type_t res_id = RpcProtoId<Res>::value;
	struct Lane
	{
		Mutex mu;
		uint64_t accept;
		uint64_t next;
		std::map<uint64_t, std::pair<sid_t, Bytes> > ready;
		Lane() : accept(1), next(1) {}
	};
	static Lane lane;
	BindRpcHandler(RpcProtoId<Req>::value, [fn, res_id](coro::Token token, SessionManager* mgr, sid_t sid, BytesStream body) -> coro::Task {
		uint64_t seq = 0;
		{
			MutexGuard lock(lane.mu);
			seq = lane.accept++;
		}
		return RunHandledRpc<Req, Res>(fn, &lane, res_id, seq, token, mgr, sid, std::move(body));
	});
}

uint64_t BindRpcCall(proto_type_t res_id, uint64_t task_id, std::coroutine_handle<> handle, int* result, int timeout_sec, std::function<bool(BytesStream&)> decode, std::function<void()> resume);
bool WakeRpcCall(uint64_t rid, int code, BytesStream* body);

// 发出请求后挂起。回包或超时回调里 Schedule 回来。回包可能比挂起先到。
template<typename Req, typename Res>
struct RpcCallAwaiter
{
	SessionManager* mgr;
	sid_t sid;
	Req* req;
	Res* res;
	int result;
	bool trace;
	std::coroutine_handle<> handle;
	std::atomic<int>* gate;

	RpcCallAwaiter()
		: mgr(NULL), sid(0), req(NULL), res(NULL), result(-1), trace(false), handle(), gate(new std::atomic<int>(0))
	{}
	RpcCallAwaiter(const RpcCallAwaiter&) = delete;
	RpcCallAwaiter(RpcCallAwaiter&& other) noexcept
		: mgr(other.mgr), sid(other.sid), req(other.req), res(other.res), result(other.result), trace(other.trace), handle(other.handle), gate(other.gate)
	{
		other.gate = NULL;
		other.handle = nullptr;
	}
	~RpcCallAwaiter() { delete gate; }

	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> h)
	{
		handle = h;
		result = -1;
		gate->store(0, std::memory_order_relaxed);
		BytesStream body;
		req->serialize(body);
		BytesStream frame = EncodePbFrame(RpcProtoId<Req>::value, body);
		static Mutex send_order;
		{
			MutexGuard order(send_order);
			BindRpcCall(RpcProtoId<Res>::value, h.promise().uid, h, &result, 180, [this](BytesStream& in) {
				try
				{
					res->deserialize(in);
					result = 0;
					return true;
				}
				catch (ParserException&)
				{
					result = -1;
					return false;
				}
			}, [this]() {
				if (trace)
				{
					coro::NotePollWake();
				}
				int prev = gate->exchange(1, std::memory_order_acq_rel);
				if (prev == 2)
				{
					coro::Schedule(handle);
				}
			});
			mgr->SendBytes(sid, frame.GetMutableBuff());
		}
		int prev = gate->exchange(2, std::memory_order_acq_rel);
		return prev != 1;
	}
	int await_resume() const noexcept { return result; }
};

template<typename Req, typename Res>
RpcCallAwaiter<Req, Res> RpcCall(SessionManager& mgr, sid_t sid, Req& req, Res& res, bool trace = false)
{
	RpcCallAwaiter<Req, Res> awaiter;
	awaiter.mgr = &mgr;
	awaiter.sid = sid;
	awaiter.req = &req;
	awaiter.res = &res;
	awaiter.result = -1;
	awaiter.trace = trace;
	awaiter.handle = {};
	return awaiter;
}

#define AWAIT_RPC(ret, req, res) \
	int ret = co_await ::coro::AwaitRpc((req), (res))

#define AWAIT_RID(ret, rid, res) \
	int ret = co_await ::coro::WaitRid((rid), (res))

#define AWAIT_RPC_THROW(req, res) \
	do { \
		int _rpc_ret = co_await ::coro::AwaitRpc((req), (res)); \
		if (_rpc_ret != 0) throw ::coro::RpcError(_rpc_ret); \
	} while (0)
