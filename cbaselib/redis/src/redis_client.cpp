#include "redis_client.h"
#include "redis_client_session.h"
#include "rpc_coro.h"
#include "poll.h"
#include "base_timer_manager.h"
#include "config.h"
#include "logger.h"
#include <atomic>
#include <deque>
#include <utility>

namespace
{
struct ReplyFree
{
	void operator()(redisReply* reply) const
	{
		freeReplyObject(reply);
	}
};

sid_t g_sid = INVALID_SID;
}

struct RedisCall::State
{
	std::vector<std::string> args;
	std::shared_ptr<redisReply> reply;
	bool timeout;
	bool ready;
	bool sync;
	RedisResult* out;
	std::coroutine_handle<> handle;
	std::atomic<int> gate;
	std::atomic<int> won;
	std::atomic<int> refs;
	Mutex mu;
	MutexCond cv;
	State() : timeout(false), ready(false), sync(false), out(NULL), gate(0), won(0), refs(1) {}
};

RedisClient RedisClient::inst;

RedisClient::RedisClient() : _reader(NULL), _timeout_ms(25000)
{
}

RedisClient::~RedisClient()
{
	if (_reader)
	{
		redisReaderFree(_reader);
		_reader = NULL;
	}
}

bool RedisClient::Init()
{
	TryGetConfig("RedisClient.timeout_ms", _timeout_ms);
	if (_timeout_ms <= 0)
	{
		_timeout_ms = 25000;
	}
	return ProtoClientManager::Init();
}

Session* RedisClient::CloneSession()
{
	return new RedisClientSession(this);
}

void RedisClient::OnClientConnect(Session* pSession)
{
	g_sid = pSession->GetSessionID();
	if (_reader)
	{
		redisReaderFree(_reader);
	}
	_reader = redisReaderCreate();
	LOG_TRACE("RedisClient::OnClientConnect sid=%d", g_sid);
}

void RedisClient::OnClientDisconnect(const Session* pSession)
{
	LOG_TRACE("RedisClient::OnClientDisconnect sid=%d", pSession->GetSessionID());
	g_sid = INVALID_SID;
	FailAll();
	Reconnect();
}

void RedisClient::OnAbortSession()
{
	LOG_TRACE("RedisClient::OnAbortSession");
	Reconnect();
}

void RedisClient::Release(RedisCall::State* st)
{
	if (!st)
	{
		return;
	}
	if (st->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete st;
	}
}

RedisCall::RedisCall(std::vector<std::string> args, RedisResult* out) : _state(new State())
{
	_state->args = std::move(args);
	_state->out = out;
}

RedisCall::RedisCall(RedisCall&& other) noexcept : _state(other._state)
{
	other._state = NULL;
}

RedisCall::~RedisCall()
{
	RedisClient::Release(_state);
}

void RedisCall::await_resume()
{
	if (_state->out)
	{
		_state->out->reply = _state->reply;
		_state->out->timeout = _state->timeout;
	}
}

bool RedisCall::Suspend(std::coroutine_handle<> h)
{
	_state->handle = h;
	Poll::GetInstance()->CallOnLoop(RedisClient::Submit, _state);
	int prev = _state->gate.exchange(2, std::memory_order_acq_rel);
	return prev != 1;
}

RedisCall RedisClient::Call(std::vector<std::string> args, RedisResult* out)
{
	return RedisCall(std::move(args), out);
}

RedisResult RedisClient::CallSync(std::vector<std::string> args)
{
	RedisCall::State* st = new RedisCall::State();
	st->args = std::move(args);
	st->sync = true;
	Poll::GetInstance()->CallOnLoop(Submit, st);
#ifndef _SINGLE_THREAD_
	{
		MutexGuard lock(st->mu);
		while (!st->ready)
		{
			st->cv.Wait(st->mu);
		}
	}
#endif
	RedisResult out;
	out.reply = st->reply;
	out.timeout = st->timeout || !st->ready;
	Release(st);
	return out;
}

void RedisClient::Submit(void* state)
{
	GetInstance().Enqueue((RedisCall::State*)state);
}

void RedisClient::FireTimeout(void* state)
{
	RedisCall::State* st = (RedisCall::State*)state;
	GetInstance().Settle(st, std::shared_ptr<redisReply>(), true);
	Release(st);
}

void RedisClient::Settle(RedisCall::State* st, std::shared_ptr<redisReply> reply, bool timeout)
{
	int expect = 0;
	if (!st->won.compare_exchange_strong(expect, 1, std::memory_order_acq_rel))
	{
		return;
	}
	{
		MutexGuard lock(st->mu);
		st->reply = std::move(reply);
		st->timeout = timeout;
		st->ready = true;
		if (st->sync)
		{
			st->cv.Signal();
		}
	}
	if (st->sync)
	{
		return;
	}
	int prev = st->gate.exchange(1, std::memory_order_acq_rel);
	if (prev == 2 && st->handle)
	{
		coro::Schedule(st->handle);
	}
}

void RedisClient::Enqueue(RedisCall::State* st)
{
	if (st->args.empty() || !IsConnect())
	{
		Settle(st, std::shared_ptr<redisReply>(), true);
		return;
	}
	std::vector<const char*> argv(st->args.size());
	std::vector<size_t> arglen(st->args.size());
	for (size_t i = 0; i < st->args.size(); ++i)
	{
		argv[i] = st->args[i].c_str();
		arglen[i] = st->args[i].size();
	}
	char* cmd = NULL;
	long long n = redisFormatCommandArgv(&cmd, (int)argv.size(), argv.data(), arglen.data());
	if (n < 0 || !cmd)
	{
		Settle(st, std::shared_ptr<redisReply>(), true);
		return;
	}
	Bytes frame;
	frame.Append(cmd, (size_t)n);
	redisFreeCommand(cmd);
	st->refs.fetch_add(1, std::memory_order_relaxed);
	{
		MutexGuard lock(_wait_mu);
		_wait.push_back(st);
	}
	if (!SendRaw(frame))
	{
		MutexGuard lock(_wait_mu);
		for (std::deque<RedisCall::State*>::iterator it = _wait.begin(); it != _wait.end(); ++it)
		{
			if (*it == st)
			{
				_wait.erase(it);
				break;
			}
		}
		Settle(st, std::shared_ptr<redisReply>(), true);
		Release(st);
		return;
	}
	st->refs.fetch_add(1, std::memory_order_relaxed);
	int wait_ms = _timeout_ms;
	BaseTimerManager::GetInstance()->AddTimer([st]() {
		Poll::GetInstance()->CallOnLoop(RedisClient::FireTimeout, st);
	}, wait_ms);
}

void RedisClient::FailAll()
{
	std::deque<RedisCall::State*> left;
	{
		MutexGuard lock(_wait_mu);
		left.swap(_wait);
	}
	for (size_t i = 0; i < left.size(); ++i)
	{
		if (left[i]->won.load(std::memory_order_acquire) == 0)
		{
			Settle(left[i], std::shared_ptr<redisReply>(), true);
		}
		Release(left[i]);
	}
}

void RedisClient::Feed(const char* data, size_t size)
{
	if (!_reader)
	{
		_reader = redisReaderCreate();
	}
	if (redisReaderFeed(_reader, data, size) != REDIS_OK)
	{
		FailAll();
		if (g_sid != INVALID_SID)
		{
			CloseSession(g_sid);
		}
		return;
	}
	for (;;)
	{
		void* raw = NULL;
		if (redisReaderGetReply(_reader, &raw) != REDIS_OK)
		{
			FailAll();
			if (g_sid != INVALID_SID)
			{
				CloseSession(g_sid);
			}
			return;
		}
		if (!raw)
		{
			return;
		}
		std::shared_ptr<redisReply> held((redisReply*)raw, ReplyFree());
		RedisCall::State* st = NULL;
		{
			MutexGuard lock(_wait_mu);
			if (_wait.empty())
			{
				continue;
			}
			st = _wait.front();
			_wait.pop_front();
		}
		if (st->won.load(std::memory_order_acquire) == 0)
		{
			Settle(st, std::move(held), false);
		}
		Release(st);
	}
}
