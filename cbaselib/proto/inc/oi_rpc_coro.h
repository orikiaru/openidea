#pragma once
#include "oi_bytes_stream.h"
#include "oi_header.h"
#include "oi_parser.h"
#include "oi_rpc_call.h"
#include "oi_session_manager.h"
#include "oi_lock.h"
#include "oi_duration.h"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <chrono>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace coro
{
// 协程 token。StartTask 生成并传进协程。
typedef uint64_t Token;
typedef Token TaskUid;

TaskUid AllocTaskUid();

// 挂起的协程放回运行队列。单线程进本线程队列，多线程进工人队列。RPC 回包和定时器都走这里。
void Schedule(std::coroutine_handle<> handle);

// 根协程跑完后不能在 final_suspend 里销毁自己。登记下来，等 resume 返回再释放。
void DetachFinished(std::coroutine_handle<> handle);
void DrainDetached();

// 单线程就绪队列。Poll 只跑进入时已经在队列里的句柄，这一轮新挂上的留到 poll 之后。
class StScheduler
{
public:
	static void Post(std::coroutine_handle<> handle);
	static bool Poll();
	static bool Queued();
};

// 下一轮再跑，或者过 ms 毫秒再跑。到点就是 Schedule。
struct SleepMs
{
	int32_t ms;
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) const noexcept;
	void await_resume() const noexcept {}
};

inline SleepMs yield(int32_t ms)
{
	return SleepMs{ms};
}

inline SleepMs yield(std::chrono::milliseconds delay)
{
	if (delay.count() > (std::numeric_limits<int32_t>::max)()) throw std::out_of_range("yield duration too large");
	return yield(delay.count() <= 0 ? 0 : static_cast<int32_t>(delay.count()));
}

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
	// 保活 StartTask 的闭包；协程 lambda 的捕获仍属于闭包对象。
	std::shared_ptr<void> callable;
	Token uid;
	std::coroutine_handle<> continuation;
	bool detached;
	std::atomic<bool> ready;
	TaskState() : uid(0), continuation(), detached(false), ready(false) {}
	std::suspend_always initial_suspend() noexcept { return {}; }
	TaskFinalSuspend final_suspend() noexcept { return {}; }
	void return_void() {}
	void unhandled_exception() { std::terminate(); }
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
	auto callable = std::make_shared<std::decay_t<F>>(std::forward<F>(fn));
	auto task = (*callable)(token);
	task.handle.promise().callable = std::move(callable);
	task.handle.promise().uid = token;
	task.Start();
	return token;
}

}

namespace OI { namespace Rpc {
template<typename T>
struct RpcProtoId;

void BindRpcHandler(proto_type_t id, std::function<coro::Task(coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> handle);

// 请求和回包的包体都以 8 字节调用号开头，后面才是 Req/Res。
// 客户端按调用号认回包，服务端处理完就回，不用按接受顺序排队。

// 业务任务外面这一层：解包，co_await 业务任务，再把带调用号的回包写出去。
template<typename Req, typename Res, typename F>
coro::Task RunHandledRpc(F fn, proto_type_t res_id, coro::Token token, ::OI::Net::SessionManager* mgr, sid_t sid, ::OI::Base::BytesStream body)
{
	Req req;
	Res res;
	try
	{
		uint64_t call_id = 0;
		body >> call_id;
		req.deserialize(body);
		auto child = fn(token, mgr, sid, req, res);
		if (child.handle)
		{
			child.handle.promise().uid = token;
		}
		co_await child;
		::OI::Base::BytesStream out;
		out << call_id;
		res.serialize(out);
		::OI::Base::BytesStream frame = EncodePbFrame(res_id, out);
		mgr->SendBytes(sid, frame.GetMutableBuff());
	}
	catch (::OI::Base::ParserException&)
	{
	}
	co_return;
}

template<typename Req, typename Res, typename F>
void HandleRpc(F fn)
{
	const proto_type_t res_id = RpcProtoId<Res>::value;
	BindRpcHandler(RpcProtoId<Req>::value, [fn, res_id](coro::Token token, ::OI::Net::SessionManager* mgr, sid_t sid, ::OI::Base::BytesStream body) -> coro::Task {
		return RunHandledRpc<Req, Res>(fn, res_id, token, mgr, sid, std::move(body));
	});
}

// 登记一种回包协议号。这种包按包体开头的调用号去找等待的调用。
void RegisterRpcRes(proto_type_t res_id);
// 返回调用号。回包、超时或 WakeRpcCall 三者只有一个会触发 decode/resume。
uint64_t BindRpcCall(::OI::Net::SessionManager* mgr, sid_t sid, proto_type_t res_id, std::coroutine_handle<> handle, int32_t* result, int64_t timeout_ms, std::function<bool(::OI::Base::BytesStream&)> decode, std::function<void()> resume);
void FailRpcCalls(::OI::Net::SessionManager* mgr, sid_t sid);
bool WakeRpcCall(uint64_t call_id, int32_t code, ::OI::Base::BytesStream* body);

// 发出请求后挂起。回包或超时回调里 Schedule 回来。回包可能比挂起先到。
template<typename Req, typename Res>
struct RpcCallAwaiter
{
	::OI::Net::SessionManager* mgr;
	sid_t sid;
	Req* req;
	Res* res;
	int32_t result;
	int64_t timeout_ms;
	std::coroutine_handle<> handle;
	std::atomic<int32_t> gate;
	// 回包原样留到 await_resume 再解，解包跑在工人上，不占 IO 线程。
	::OI::Base::BytesStream reply;
	bool replied;

	RpcCallAwaiter()
		: mgr(NULL), sid(0), req(NULL), res(NULL), result(-1), timeout_ms(180000), handle(), gate(0), replied(false)
	{}
	RpcCallAwaiter(const RpcCallAwaiter&) = delete;
	RpcCallAwaiter(RpcCallAwaiter&& other) noexcept
		: mgr(other.mgr), sid(other.sid), req(other.req), res(other.res), result(other.result),
		  timeout_ms(other.timeout_ms), handle(other.handle), gate(other.gate.load(std::memory_order_relaxed)),
		  replied(false)
	{
		other.handle = nullptr;
	}

	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> h)
	{
		static const bool registered = (RegisterRpcRes(RpcProtoId<Res>::value), true);
		(void)registered;
		handle = h;
		result = -1;
		replied = false;
		gate.store(0, std::memory_order_relaxed);
		uint64_t call_id = BindRpcCall(mgr, sid, RpcProtoId<Res>::value, h, &result, timeout_ms, [this](::OI::Base::BytesStream& in) {
			reply = std::move(in);
			replied = true;
			return true;
		}, [this]() {
			int prev = gate.exchange(1, std::memory_order_acq_rel);
			if (prev == 2)
			{
				coro::Schedule(handle);
			}
		});
		::OI::Base::BytesStream body;
		body << call_id;
		req->serialize(body);
		::OI::Base::BytesStream frame = EncodePbFrame(RpcProtoId<Req>::value, body);
		if (!mgr->SendBytes(sid, frame.GetMutableBuff()))
		{
			WakeRpcCall(call_id, -1, NULL);
		}
		int prev = gate.exchange(2, std::memory_order_acq_rel);
		return prev != 1;
	}
	int await_resume()
	{
		if (replied)
		{
			try
			{
				res->deserialize(reply);
				result = 0;
			}
			catch (::OI::Base::ParserException&)
			{
				result = -1;
			}
		}
		return result;
	}
};

template<typename Req, typename Res>
RpcCallAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& mgr, sid_t sid, Req& req, Res& res, int32_t timeout_sec = 180)
{
	RpcCallAwaiter<Req, Res> awaiter;
	awaiter.mgr = &mgr;
	awaiter.sid = sid;
	awaiter.req = &req;
	awaiter.res = &res;
	awaiter.result = -1;
	awaiter.timeout_ms = static_cast<int64_t>(timeout_sec) * 1000;
	awaiter.handle = {};
	return awaiter;
}

template<typename Req, typename Res>
RpcCallAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& mgr, sid_t sid, Req& req, Res& res, std::chrono::milliseconds timeout)
{
	auto awaiter = RpcCall(mgr, sid, req, res);
	awaiter.timeout_ms = timeout.count();
	return awaiter;
}

template<typename Res>
struct [[nodiscard]] RpcResult
{
	int code;
	Res value;
	explicit operator bool() const { return code == 0; }
};

// 拥有请求和响应，应用不必跨挂起维护输出引用。
template<typename Req, typename Res>
struct RpcValueAwaiter : RpcCallAwaiter<Req, Res>
{
	Req request;
	Res response{};
	RpcValueAwaiter(::OI::Net::SessionManager& mgr, sid_t sid, Req req, std::chrono::milliseconds timeout)
		: request(std::move(req))
	{
		this->mgr = &mgr;
		this->sid = sid;
		this->timeout_ms = timeout.count();
	}
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> h)
	{
		this->req = &request;
		this->res = &response;
		return RpcCallAwaiter<Req, Res>::await_suspend(h);
	}
	RpcResult<Res> await_resume()
	{
		return {RpcCallAwaiter<Req, Res>::await_resume(), std::move(response)};
	}
};

template<typename Res, typename Req>
RpcValueAwaiter<Req, Res> RpcCall(::OI::Net::SessionManager& mgr, sid_t sid, Req req, std::chrono::milliseconds timeout = std::chrono::seconds(180))
{
	return RpcValueAwaiter<Req, Res>(mgr, sid, std::move(req), timeout);
}
}}
