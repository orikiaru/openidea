#pragma once
#include "oi_bytes_stream.h"
#include "oi_header.h"
#include "oi_parser.h"
#include "oi_rpc_call.h"
#include "oi_session_manager.h"
#include "oi_rpc_session_manager.h"
#include "oi_lock.h"
#include "oi_duration.h"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
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
using TaskErrorHandler = void (*)(Token, std::exception_ptr);
// 在工作线程调用；处理函数必须线程安全，不允许抛异常。nullptr 恢复默认 stderr 报告。
void SetTaskErrorHandler(TaskErrorHandler handler);
void ReportTaskError(Token token, std::exception_ptr error) noexcept;

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
	std::exception_ptr error;
	Token uid;
	std::coroutine_handle<> continuation;
	bool detached;
	std::atomic<bool> ready;
	TaskState() : uid(0), continuation(), detached(false), ready(false) {}
	std::suspend_always initial_suspend() noexcept { return {}; }
	TaskFinalSuspend final_suspend() noexcept { return {}; }
	void unhandled_exception() noexcept
	{
		error = std::current_exception();
		if (detached) ReportTaskError(uid, error);
	}
};

// 有返回值和无返回值任务共用同一套调度与完成门，不增加包装协程。
template<typename T>
struct TaskReturn
{
	std::optional<T> result;
	void return_value(T value) { result.emplace(std::move(value)); }
};
template<>
struct TaskReturn<void>
{
	void return_void() noexcept {}
};

// 一条任务。Start 丢进队列；co_await 就是调用它，跑完回到调用方。
template<typename T = void>
struct [[nodiscard]] BasicTask
{
	struct promise_type : TaskState, TaskReturn<T>
	{
		BasicTask get_return_object()
		{
			return BasicTask{std::coroutine_handle<promise_type>::from_promise(*this)};
		}
	};
	std::coroutine_handle<promise_type> handle;
	explicit BasicTask(std::coroutine_handle<promise_type> h) : handle(h) {}
	BasicTask(BasicTask&& other) noexcept : handle(other.handle) { other.handle = {}; }
	BasicTask(const BasicTask&) = delete;
	~BasicTask() { if (handle) handle.destroy(); }
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
	T await_resume()
	{
		if (handle && handle.promise().error) std::rethrow_exception(handle.promise().error);
		if constexpr (!std::is_void_v<T>)
		{
			if (!handle || !handle.promise().result) throw std::logic_error("Task has no result");
			return std::move(*handle.promise().result);
		}
	}
};

using Task = BasicTask<void>;
template<typename T>
using TaskResult = BasicTask<T>;

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

// 协作停止：不会销毁仍被 IO/定时器引用的帧。模块销毁前必须 await Join。
class StopToken
{
	std::shared_ptr<std::atomic<bool>> _stop;
public:
	explicit StopToken(std::shared_ptr<std::atomic<bool>> stop) : _stop(std::move(stop)) {}
	bool StopRequested() const { return _stop->load(std::memory_order_acquire); }
};

class TaskScope
{
	struct State
	{
		::OI::Thread::Mutex mutex;
		std::shared_ptr<std::atomic<bool>> stop = std::make_shared<std::atomic<bool>>(false);
		size_t active = 0;
		std::vector<std::coroutine_handle<>> waiters;
		bool closed = false;
		std::exception_ptr error;
	};
	std::shared_ptr<State> _state = std::make_shared<State>();
	template<typename F>
	static Task Run(std::shared_ptr<State> state, F fn, Token token)
	{
		try { co_await fn(token, StopToken(state->stop)); }
		catch (...)
		{
			::OI::Thread::MutexGuard lock(state->mutex);
			if (!state->error) state->error = std::current_exception();
			state->stop->store(true, std::memory_order_release);
		}
		std::vector<std::coroutine_handle<>> waiters;
		{
			::OI::Thread::MutexGuard lock(state->mutex);
			if (--state->active == 0) waiters.swap(state->waiters);
		}
		for (auto waiter : waiters) Schedule(waiter);
	}
	struct JoinAwaiter
	{
		std::shared_ptr<State> state;
		bool await_ready() const noexcept { return false; }
		bool await_suspend(std::coroutine_handle<> waiter)
		{
			::OI::Thread::MutexGuard lock(state->mutex);
			state->closed = true;
			if (state->active == 0) return false;
			state->waiters.push_back(waiter);
			return true;
		}
		void await_resume()
		{
			::OI::Thread::MutexGuard lock(state->mutex);
			if (state->error) std::rethrow_exception(state->error);
		}
	};
	static Task Wait(std::shared_ptr<State> state)
	{
		co_await JoinAwaiter{std::move(state)};
	}
public:
	TaskScope() = default;
	TaskScope(const TaskScope&) = delete;
	TaskScope& operator=(const TaskScope&) = delete;
	~TaskScope() { RequestStop(); }
	void RequestStop()
	{
		::OI::Thread::MutexGuard lock(_state->mutex);
		_state->closed = true;
		_state->stop->store(true, std::memory_order_release);
	}
	template<typename F>
	Token Start(F&& fn)
	{
		::OI::Thread::MutexGuard lock(_state->mutex);
		if (_state->closed) throw std::logic_error("TaskScope is closed");
		Token token = AllocTaskUid();
		auto task = Run(_state, std::forward<F>(fn), token);
		task.handle.promise().uid = token;
		++_state->active;
		return task.Start();
	}
	Task Join() { return Wait(_state); }
};

}

namespace OI { namespace Rpc {
// 标准服务端协程：co_return 0 成功，正数为业务错误。
using RpcTask = coro::TaskResult<int32_t>;

// 仅需要连接信息的处理函数接收上下文；生命周期覆盖本次处理协程。
struct RpcContext
{
	coro::Token token;
	::OI::Net::SessionManager& manager;
	sid_t sid;
};

template<typename T>
struct RpcProtoId;

void BindRpcHandler(proto_type_t id, std::function<coro::Task(coro::Token, ::OI::Net::SessionManager*, sid_t, ::OI::Base::BytesStream)> handle);

// 请求和回包共用一个协议号。调用号最高位是 1 表示请求，回包清掉这一位。
// 请求：调用号 + Req；回执：调用号 + int32_t retcode + 成功时的 Res。
// 0 成功，负数保留给框架错误，正数用于业务错误。失败不序列化 Res。

// 业务任务外面这一层：解包，co_await 业务任务，再把带调用号的回包写出去。
template<typename Req, typename Res, typename F>
coro::Task RunHandledRpc(F fn, proto_type_t res_id, coro::Token token, ::OI::Net::SessionManager* mgr, sid_t sid, ::OI::Base::BytesStream body)
{
	uint64_t call_id = 0;
	try { body >> call_id; }
	catch (::OI::Base::ParserException&) { co_return; }
	call_id &= ~(uint64_t)RPC_REQ_MASK;
	Req req{};
	Res res{};
	int32_t retcode = 0;
	try
	{
		req.deserialize(body);
		RpcContext context{token, *mgr, sid};
		auto invoke = [&]() {
			if constexpr (std::is_invocable_v<F&, Req&, Res&>)
				return fn(req, res);
			else if constexpr (std::is_invocable_v<F&, const RpcContext&, Req&, Res&>)
				return fn(context, req, res);
			else
				return fn(token, mgr, sid, req, res); // 兼容已有五参数回调。
		};
		using Return = decltype(invoke());
		if constexpr (std::is_same_v<Return, int32_t>)
		{
			retcode = invoke();
		}
		else
		{
			static_assert(std::is_same_v<Return, RpcTask> || std::is_same_v<Return, coro::Task>,
				"RPC handler must return RpcTask, int32_t or legacy coro::Task");
			auto child = invoke();
			if (child.handle) child.handle.promise().uid = token;
			if constexpr (std::is_same_v<Return, coro::Task>) co_await child;
			else retcode = co_await child;
		}
	}
	catch (::OI::Base::ParserException&) { retcode = OI_ERR_FAIL; }
	catch (...)
	{
		retcode = OI_ERR_FAIL;
		coro::ReportTaskError(token, std::current_exception());
	}
	::OI::Base::BytesStream out;
	out << call_id << retcode;
	if (retcode == 0) res.serialize(out);
	::OI::Base::BytesStream frame = EncodePbFrame(res_id, out);
	if (!mgr->SendBytes(sid, frame.GetMutableBuff())) mgr->CloseSession(sid);
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

// 请求类型已关联响应，无需在注册处重复指定。
template<typename Req, typename F>
void HandleRpc(F fn)
{
	HandleRpc<Req, typename RpcProtoId<Req>::Response>(std::move(fn));
}

// 返回调用号。回包、超时或 WakeRpcCall 三者只有一个会触发 decode/resume。
uint64_t BindRpcCall(::OI::Net::SessionManager* mgr, sid_t sid, proto_type_t res_id, std::coroutine_handle<> handle, int32_t* result, int64_t timeout_ms, std::function<bool(::OI::Base::BytesStream&)> decode, std::function<void()> resume);
void FailRpcCalls(::OI::Net::SessionManager* mgr, sid_t sid);
bool WakeRpcCall(uint64_t call_id, int32_t code, ::OI::Base::BytesStream* body);

// 发出请求后挂起。回包或超时回调里 Schedule 回来。回包可能比挂起先到。
template<typename Req, typename Res>
struct [[nodiscard]] RpcCallAwaiter
{
	static_assert(RpcProtoId<Req>::value == RpcProtoId<Res>::value, "RPC request/response protocol mismatch");
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
	std::exception_ptr submission_error;

	RpcCallAwaiter()
		: mgr(NULL), sid(0), req(NULL), res(NULL), result(OI_ERR_FAIL), timeout_ms(180000), handle(), gate(0), replied(false)
	{}
	RpcCallAwaiter(const RpcCallAwaiter&) = delete;
	RpcCallAwaiter(RpcCallAwaiter&& other) noexcept
		: mgr(other.mgr), sid(other.sid), req(other.req), res(other.res), result(other.result),
		  timeout_ms(other.timeout_ms), handle(other.handle), gate(other.gate.load(std::memory_order_relaxed)),
		  replied(false), submission_error(std::move(other.submission_error))
	{
		other.handle = nullptr;
	}

	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> h)
	{
		handle = h;
		result = OI_ERR_FAIL;
		replied = false;
		submission_error = nullptr;
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
		try
		{
			::OI::Base::BytesStream body;
			body << (call_id | (uint64_t)RPC_REQ_MASK);
			req->serialize(body);
			::OI::Base::BytesStream frame = EncodePbFrame(RpcProtoId<Req>::value, body);
			if (mgr->TryPendCall(call_id, frame.GetMutableBuff()))
			{
			}
			else if (!mgr->SendBytes(sid, frame.GetMutableBuff()))
			{
				WakeRpcCall(call_id, mgr->UnavailableCode(), NULL);
			}
		}
		catch (...)
		{
			// 不直接从 await_suspend 抛出：超时/断连可能已取走等待项，正在使用 this。
			// 先完成等待，再由同一 gate 确认回调已结束，最后在 await_resume 重抛。
			submission_error = std::current_exception();
			WakeRpcCall(call_id, OI_ERR_FAIL, nullptr);
		}
		int prev = gate.exchange(2, std::memory_order_acq_rel);
		return prev != 1;
	}
	int32_t await_resume()
	{
		if (submission_error) std::rethrow_exception(submission_error);
		if (replied)
		{
			try
			{
				reply >> result;
				if (result == 0)
				{
					Res decoded{};
					decoded.deserialize(reply);
					*res = std::move(decoded);
				}
			}
			catch (::OI::Base::ParserException&)
			{
				result = OI_ERR_FAIL;
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
	awaiter.result = OI_ERR_FAIL;
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
class [[nodiscard]] RpcResult
{
	int32_t _ret;
	Res _res;
	void Check() const
	{
		if (_ret != 0) throw std::logic_error("RPC failed: check GetRet() before GetRes()");
	}
public:
	RpcResult(int32_t ret, Res res) : _ret(ret), _res(std::move(res)) {}
	int32_t GetRet() const noexcept { return _ret; }
	explicit operator bool() const noexcept { return _ret == 0; }
	Res& GetRes() & { Check(); return _res; }
	const Res& GetRes() const & { Check(); return _res; }
	Res GetRes() && { Check(); return std::move(_res); }
	Res GetRes() const && { Check(); return _res; }
};

// 输出引用仅在成功解包后更新，错误时保留调用方原值。
template<typename Req>
auto RpcCall(ProtoClientManager& mgr, Req& req, typename RpcProtoId<Req>::Response& res,
	std::chrono::milliseconds timeout = ::OI::Timer::Seconds(180))
{
	return RpcCall(mgr, mgr.ConnectedSid(), req, res, timeout);
}

// 拥有请求和响应，应用不必跨挂起维护输出引用。
template<typename Req, typename Res>
struct [[nodiscard]] RpcValueAwaiter : RpcCallAwaiter<Req, Res>
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
// 自动配对响应类型；保留显式 Res 和输出引用接口供旧代码使用。
template<typename Req>
auto RpcCall(::OI::Net::SessionManager& mgr, sid_t sid, Req req,
	std::chrono::milliseconds timeout = ::OI::Timer::Seconds(180))
	-> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {mgr, sid, std::move(req), timeout};
}

template<typename Req>
auto RpcCall(ProtoClientManager& mgr, Req req,
	std::chrono::milliseconds timeout = ::OI::Timer::Seconds(180))
	-> RpcValueAwaiter<Req, typename RpcProtoId<Req>::Response>
{
	return {mgr, mgr.ConnectedSid(), std::move(req), timeout};
}

}}

// 业务命名约定：协议 Name 对应生成的 NameReq / NameRes。
// 可用于函数声明或定义；req/res/context 是宏提供的参数名。
#define OI_RPC_HANDLER(Name, Function) \
	::OI::Rpc::RpcTask Function(const ::OI::RpcData::Name##Req& req, ::OI::RpcData::Name##Res& res)
#define OI_RPC_SYNC_HANDLER(Name, Function) \
	int32_t Function(const ::OI::RpcData::Name##Req& req, ::OI::RpcData::Name##Res& res)
#define OI_RPC_CONTEXT_HANDLER(Name, Function) \
	::OI::Rpc::RpcTask Function(const ::OI::Rpc::RpcContext& context, const ::OI::RpcData::Name##Req& req, ::OI::RpcData::Name##Res& res)
#define OI_RPC_BIND(Name, ...) \
	::OI::Rpc::HandleRpc<::OI::RpcData::Name##Req>((__VA_ARGS__))
