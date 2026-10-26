#pragma once
#include "bytes_stream.h"
#include "header.h"
#include "parser.h"
#include "rpc.h"
#include "session_manager.h"
#include <atomic>
#include <coroutine>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>

namespace coro
{
enum class Kind
{
	Single,
	Multi,
};

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

// co_yield 把当前协程放回线程池队尾，下一次 FetchTask 跑别的 Task。
struct YieldToPool
{
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) const noexcept;
	void await_resume() const noexcept {}
};

// co_yield 1000 或 co_await SleepMs{1000}：挂进定时器，到点再 resume 这个句柄。
struct SleepMs
{
	int ms;
	bool await_ready() const noexcept { return false; }
	void await_suspend(std::coroutine_handle<> handle) const noexcept;
	void await_resume() const noexcept {}
};

class StScheduler
{
public:
	static void Post(std::coroutine_handle<> handle);
	static bool Poll();
};

class MtScheduler
{
public:
	static void Post(std::coroutine_handle<> handle);
	static void Start();
};

template<typename Req, typename Res>
struct RpcAwaiter
{
	const Req* req;
	Res* res;
	int result;
	Kind kind;
	uint64_t id;
	std::coroutine_handle<> handle;

	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	void await_suspend(std::coroutine_handle<Promise> h)
	{
		handle = h;
		kind = Promise::kind;
		result = -1;
		id = Hub::Add([this](int code, const void* value) {
			result = code;
			if (code == 0 && value && res)
			{
				*res = *static_cast<const Res*>(value);
			}
			if (kind == Kind::Single)
			{
				StScheduler::Post(handle);
			}
			else
			{
				MtScheduler::Post(handle);
			}
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
	Kind kind;
	std::coroutine_handle<> handle;

	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	void await_suspend(std::coroutine_handle<Promise> h)
	{
		handle = h;
		kind = Promise::kind;
		result = -1;
		uint64_t key = rid & ~((uint64_t)RPC_REQ_MASK);
		Hub::Bind(key, [this](int code, const void* value) {
			result = code;
			if (code == 0 && value && res)
			{
				BytesStream* in = (BytesStream*)value;
				(*in) >> *res;
			}
			if (kind == Kind::Single)
			{
				StScheduler::Post(handle);
			}
			else
			{
				MtScheduler::Post(handle);
			}
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
	awaiter.kind = Kind::Single;
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
	awaiter.kind = Kind::Single;
	awaiter.id = 0;
	awaiter.handle = {};
	return awaiter;
}

struct StTask
{
	struct promise_type
	{
		static constexpr Kind kind = Kind::Single;
		StTask get_return_object()
		{
			return StTask{std::coroutine_handle<promise_type>::from_promise(*this)};
		}
		std::suspend_always initial_suspend() noexcept { return {}; }
		std::suspend_always final_suspend() noexcept { return {}; }
		void return_void() {}
		void unhandled_exception() { std::terminate(); }
		SleepMs yield_value(int ms) noexcept { return SleepMs{ms}; }
		template<typename T>
		YieldToPool yield_value(T&&) noexcept { return {}; }
	};
	std::coroutine_handle<promise_type> handle;
	explicit StTask(std::coroutine_handle<promise_type> h) : handle(h) {}
	StTask(StTask&& other) noexcept : handle(other.handle) { other.handle = {}; }
	StTask(const StTask&) = delete;
	~StTask() { if (handle) handle.destroy(); }
	void Start()
	{
		std::coroutine_handle<> h = handle;
		handle = nullptr;
		StScheduler::Post(h);
	}
};

struct MtTask
{
	struct promise_type
	{
		static constexpr Kind kind = Kind::Multi;
		MtTask get_return_object()
		{
			return MtTask{std::coroutine_handle<promise_type>::from_promise(*this)};
		}
		std::suspend_always initial_suspend() noexcept { return {}; }
		std::suspend_always final_suspend() noexcept { return {}; }
		void return_void() {}
		void unhandled_exception() { std::terminate(); }
		SleepMs yield_value(int ms) noexcept { return SleepMs{ms}; }
		template<typename T>
		YieldToPool yield_value(T&&) noexcept { return {}; }
	};
	std::coroutine_handle<promise_type> handle;
	explicit MtTask(std::coroutine_handle<promise_type> h) : handle(h) {}
	MtTask(MtTask&& other) noexcept : handle(other.handle) { other.handle = {}; }
	MtTask(const MtTask&) = delete;
	~MtTask() { if (handle) handle.destroy(); }
	void Start()
	{
		std::coroutine_handle<> h = handle;
		handle = nullptr;
		MtScheduler::Post(h);
	}
};
}

// rid 对应这次挂起的协程。回包和超时都用这张表里的句柄唤醒。
uint64_t BindRpcCall(proto_type_t res_id, std::coroutine_handle<> handle, int* result, int timeout_sec, std::function<bool(BytesStream&)> decode, std::function<void()> resume);
bool WakeRpcCall(uint64_t rid, int code, BytesStream* body);

template<typename T>
struct RpcProtoId;

template<typename Req, typename Res>
struct RpcCallAwaiter
{
	SessionManager* mgr;
	sid_t sid;
	Req* req;
	Res* res;
	int result;
	coro::Kind kind;
	std::coroutine_handle<> handle;
	std::atomic<int>* gate;

	RpcCallAwaiter()
		: mgr(NULL), sid(0), req(NULL), res(NULL), result(-1), kind(coro::Kind::Single), handle(), gate(new std::atomic<int>(0))
	{}
	RpcCallAwaiter(const RpcCallAwaiter&) = delete;
	RpcCallAwaiter(RpcCallAwaiter&& other) noexcept
		: mgr(other.mgr), sid(other.sid), req(other.req), res(other.res), result(other.result), kind(other.kind), handle(other.handle), gate(other.gate)
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
		kind = Promise::kind;
		result = -1;
		gate->store(0, std::memory_order_relaxed);
		BytesStream body;
		req->serialize(body);
		BytesStream frame = EncodePbFrame(RpcProtoId<Req>::value, body);
		static std::mutex send_order;
		{
			std::lock_guard<std::mutex> order(send_order);
			BindRpcCall(RpcProtoId<Res>::value, h, &result, 180, [this](BytesStream& in) {
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
				int prev = gate->exchange(1, std::memory_order_acq_rel);
				if (prev == 2)
				{
					coro::StScheduler::Post(handle);
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
RpcCallAwaiter<Req, Res> RpcCall(SessionManager& mgr, sid_t sid, Req& req, Res& res)
{
	RpcCallAwaiter<Req, Res> awaiter;
	awaiter.mgr = &mgr;
	awaiter.sid = sid;
	awaiter.req = &req;
	awaiter.res = &res;
	awaiter.result = -1;
	awaiter.kind = coro::Kind::Single;
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
