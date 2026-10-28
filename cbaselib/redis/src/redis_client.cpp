#include "redis_client.h"
#include "redis_client_session.h"
#include "rpc_coro.h"
#include "poll.h"
#include "base_timer_manager.h"
#include "config.h"
#include "logger.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
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
std::once_flag g_sweep_once;
const int kSweepMs = 200;

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

class RedisSweepTimer : public BaseTimer
{
public:
	bool OnTimer()
	{
		Poll::GetInstance()->CallOnLoop(RedisClient::SweepTimeout, NULL);
		return true;
	}
};
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
	int64_t deadline_ms;
	std::atomic<int> gate;
	std::atomic<int> won;
	std::atomic<int> refs;
	Mutex mu;
	MutexCond cv;
	State() : timeout(false), ready(false), sync(false), out(NULL), deadline_ms(0), gate(0), won(0), refs(1) {}
};

RedisClient RedisClient::inst;

RedisClient::RedisClient() : _reader(NULL), _timeout_ms(25000), _swept(0)
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
#ifdef _SINGLE_THREAD_
	// 同步等回包。先把已经挂上的协程跑一轮，再 poll；没有别人在等就堵住。
	while (!st->ready)
	{
		coro::StScheduler::Poll();
		if (st->ready)
		{
			break;
		}
		Poll::GetInstance()->PollWait(coro::StScheduler::Queued() ? 0 : 1);
	}
#else
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

void RedisClient::SweepTimeout(void*)
{
	RedisClient& self = GetInstance();
	int64_t now = NowMs();
	std::vector<RedisCall::State*> expired;
	{
		// 超时时长固定，又按发送顺序入队，所以 _wait 按截止时间有序。
		// 超时的仍留在队里认领它那条回执，_swept 记着已经判过的前缀，不重复扫。
		MutexGuard lock(self._wait_mu);
		while (self._swept < self._wait.size())
		{
			RedisCall::State* st = self._wait[self._swept];
			if (st->deadline_ms > now)
			{
				break;
			}
			expired.push_back(st);
			++self._swept;
		}
	}
	for (size_t i = 0; i < expired.size(); ++i)
	{
		self.Settle(expired[i], std::shared_ptr<redisReply>(), true);
	}
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
	std::call_once(g_sweep_once, []() {
		BaseTimerManager::GetInstance()->AddTimer(new RedisSweepTimer(), kSweepMs);
	});
	st->deadline_ms = NowMs() + _timeout_ms;
	st->refs.fetch_add(1, std::memory_order_relaxed);
	{
		MutexGuard lock(_wait_mu);
		_wait.push_back(st);
	}
	if (!SendRaw(frame))
	{
		MutexGuard lock(_wait_mu);
		for (size_t i = 0; i < _wait.size(); ++i)
		{
			if (_wait[i] == st)
			{
				_wait.erase(_wait.begin() + (std::ptrdiff_t)i);
				if (i < _swept)
				{
					--_swept;
				}
				break;
			}
		}
		Settle(st, std::shared_ptr<redisReply>(), true);
		Release(st);
		return;
	}
}

void RedisClient::FailAll()
{
	std::deque<RedisCall::State*> left;
	{
		MutexGuard lock(_wait_mu);
		left.swap(_wait);
		_swept = 0;
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
			if (_swept > 0)
			{
				--_swept;
			}
		}
		if (st->won.load(std::memory_order_acquire) == 0)
		{
			Settle(st, std::move(held), false);
		}
		Release(st);
	}
}
