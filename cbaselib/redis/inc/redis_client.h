#pragma once
#include "hiredis.h"
#include "proto_session_manager.h"
#include <coroutine>
#include <deque>
#include <memory>
#include <string>
#include <vector>

struct redisReader;

// reply 非空是回执，timeout 为真是超时。先到的那个算数。
struct RedisResult
{
	std::shared_ptr<redisReply> reply;
	bool timeout;
	RedisResult() : timeout(false) {}
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
	void await_resume();
private:
	bool Suspend(std::coroutine_handle<> h);
};

// 跟日志一样走自己的连接，字节是 RESP，不进 proto。
class RedisClient : public ProtoClientManager
{
	friend class RedisCall;
	static RedisClient inst;
	redisReader* _reader;
	int _timeout_ms;
	Mutex _wait_mu;
	std::deque<RedisCall::State*> _wait;
public:
	RedisClient();
	virtual ~RedisClient();
	static RedisClient& GetInstance() { return inst; }
	virtual const std::string GetIDString() { return "RedisClient"; }
	virtual bool Init();
	RedisCall Call(std::vector<std::string> args, RedisResult* out);
	RedisResult CallSync(std::vector<std::string> args);
	void Feed(const char* data, size_t size);
private:
	virtual Session* CloneSession();
	virtual void OnClientConnect(Session* pSession);
	virtual void OnClientDisconnect(const Session* pSession);
	virtual void OnAbortSession();
	static void Submit(void* state);
	static void FireTimeout(void* state);
	void Enqueue(RedisCall::State* st);
	void FailAll();
	void Settle(RedisCall::State* st, std::shared_ptr<redisReply> reply, bool timeout);
	static void Release(RedisCall::State* st);
};

inline RedisCall CallRedis(std::vector<std::string> args, RedisResult& out)
{
	return RedisClient::GetInstance().Call(std::move(args), &out);
}

inline RedisResult CallRedisSync(std::vector<std::string> args)
{
	return RedisClient::GetInstance().CallSync(std::move(args));
}

#include "redis_cmd.h"
