#pragma once
#include "hiredis.h"
#include "oi_proto_id.h"
#include "oi_rpc_session_manager.h"
#include "oi_singleton.h"
#include <coroutine>
#include <deque>
#include <memory>
#include <string>
#include <vector>
namespace OI { namespace Redis {

// RESP 字节里不带这个号。号留给以后把几种定制协议放到一条连接上。
enum { REDIS_PROTO_ID = ::OI::Base::PROTO_REDIS };

enum class RedisStatus { Ok, Timeout, Disconnected, InvalidArgument, SendFailed, ServerError, ProtocolError, Busy };

// reply 共享 hiredis 回执，错误回执也保留，方便业务读取错误文本。
struct RedisResult
{
	std::shared_ptr<redisReply> reply;
	bool timeout;
	RedisStatus status;
	RedisResult() : timeout(false), status(RedisStatus::Disconnected) {}
	explicit operator bool() const { return status == RedisStatus::Ok; }
};

class RedisClient;

class RedisCall
{
	friend class RedisClient;
	struct State;
	State* _state;
public:
	RedisCall(std::vector<std::string> args, RedisResult* out);
	RedisCall(RedisCall&& other) noexcept;
	RedisCall(const RedisCall&) = delete;
	RedisCall& operator=(const RedisCall&) = delete;
	~RedisCall();
	bool await_ready() const noexcept { return false; }
	template<typename Promise>
	bool await_suspend(std::coroutine_handle<Promise> h)
	{
		return Suspend(h);
	}
	RedisResult await_resume();
private:
	bool Suspend(std::coroutine_handle<> h);
};

// 跟日志一样走自己的连接，字节是 RESP，不进 proto。
class RedisClient : public ::OI::Rpc::ProtoClientManager
{
	friend class RedisCall;
	DECLARE_SINGLETON(RedisClient)
	RedisClient();
	redisReader* _reader;
	int32_t _timeout_ms;
	int32_t _max_pending;
	::OI::Thread::Mutex _wait_mu;
	std::deque<RedisCall::State*> _wait;
	// _wait 前面这么多条已经判过超时
	size_t _swept;
public:
	virtual ~RedisClient();
	virtual const std::string GetIDString() { return "RedisClient"; }
	virtual bool Init();
	RedisCall Call(std::vector<std::string> args, RedisResult* out);
	// 阻塞到回执或超时。多线程不能在 poll 线程上调。
	// 单线程没有第二个人收包，调用方就在这条线程上把 poll 和协程跑到回执为止。
	RedisResult CallSync(std::vector<std::string> args);
	size_t PendingCount()
	{
		::OI::Thread::MutexGuard lock(_wait_mu);
		return _wait.size();
	}
	void Feed(const char* data, size_t size);
	// poll 线程上周期调用，把到期还没回执的调用按超时结掉
	static void SweepTimeout(void*);
private:
	virtual ::OI::Net::Session* CloneSession();
	virtual void OnClientConnect(::OI::Net::Session* pSession);
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession);
	static void Submit(void* state);
	void Enqueue(RedisCall::State* st);
	void FailAll(RedisStatus status = RedisStatus::Disconnected);
	void FailSid(sid_t sid);
	void Settle(RedisCall::State* st, std::shared_ptr<redisReply> reply, RedisStatus status);
	static void Release(RedisCall::State* st);
};

inline RedisCall CallRedis(std::vector<std::string> args, RedisResult& out)
{
	return RedisClient::GetInstance().Call(std::move(args), &out);
}

inline RedisCall CallRedis(std::vector<std::string> args)
{
	return RedisClient::GetInstance().Call(std::move(args), nullptr);
}

inline RedisResult CallRedisSync(std::vector<std::string> args)
{
	return RedisClient::GetInstance().CallSync(std::move(args));
}

}}
#include "oi_redis_cmd.h"
