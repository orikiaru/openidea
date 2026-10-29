#include "oi_redis.h"
#include "oi_redis_session.h"
#include "oi_rpc_coro.h"
#include "oi_poll.h"
#include "oi_timer_manager.h"
#include "oi_config.h"
#include "oi_log.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
namespace OI { namespace Redis {

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

class RedisSweepTimer : public ::OI::Timer::BaseTimer
{
public:
	bool OnTimer()
	{
		::OI::Net::Poll::GetInstance().CallOnLoop(RedisClient::SweepTimeout, NULL);
		return true;
	}
};
}

struct RedisCall::State
{
	std::vector<std::string> args;
	std::shared_ptr<redisReply> reply;
	RedisStatus status;
	bool ready;
	bool sync;
	RedisResult* out;
	std::coroutine_handle<> handle;
	int64_t deadline_ms;
	std::atomic<int32_t> gate;
	std::atomic<int32_t> won;
	std::atomic<int32_t> refs;
	::OI::Thread::Mutex mu;
	::OI::Thread::MutexCond cv;
	State() : status(RedisStatus::Disconnected), ready(false), sync(false), out(NULL), deadline_ms(0), gate(0), won(0), refs(1) {}
};

RedisClient::RedisClient() : _reader(NULL), _timeout_ms(25000), _max_pending(65536), _swept(0)
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
	TryGetConfig("RedisClient.max_pending", _max_pending);
	if (_max_pending < 1) return false;
	if (_timeout_ms <= 0)
	{
		_timeout_ms = 25000;
	}
	return ::OI::Rpc::ProtoClientManager::Init();
}

::OI::Net::Session* RedisClient::CloneSession()
{
	return new RedisClientSession(this);
}

void RedisClient::OnClientConnect(::OI::Net::Session* pSession)
{
	g_sid = pSession->GetSessionID();
	if (_reader)
	{
		redisReaderFree(_reader);
	}
	_reader = redisReaderCreate();
	LOG_TRACE("RedisClient::OnClientConnect sid=%d", g_sid);
}

void RedisClient::OnClientDisconnect(const ::OI::Net::Session* pSession)
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

RedisResult RedisCall::await_resume()
{
	RedisResult result;
	result.reply = _state->reply;
	result.status = _state->status;
	result.timeout = result.status == RedisStatus::Timeout;
	if (_state->out) *_state->out = result;
	return result;
}

bool RedisCall::Suspend(std::coroutine_handle<> h)
{
	_state->handle = h;
	::OI::Net::Poll::GetInstance().CallOnLoop(RedisClient::Submit, _state);
	int prev = _state->gate.exchange(2, std::memory_order_acq_rel);
	return prev != 1;
}

RedisCall RedisClient::Call(std::vector<std::string> args, RedisResult* out)
{
	return RedisCall(std::move(args), out);
}

RedisResult RedisClient::CallSync(std::vector<std::string> args)
{
#ifndef _SINGLE_THREAD_
	// 回包和超时都在 poll 上完成，不能在这里阻塞 poll 等自己。
	if (::OI::Net::Poll::GetInstance().InLoopThread())
	{
		throw std::logic_error("CallRedisSync cannot run on the poll thread; use co_await CallRedis");
	}
#endif
	RedisCall::State* st = new RedisCall::State();
	st->args = std::move(args);
	st->sync = true;
	::OI::Net::Poll::GetInstance().CallOnLoop(Submit, st);
#ifdef _SINGLE_THREAD_
	// 同步等回包。先把已经挂上的协程跑一轮，再 poll；没有别人在等就堵住。
	while (!st->ready)
	{
		coro::StScheduler::Poll();
		if (st->ready)
		{
			break;
		}
		::OI::Net::Poll::GetInstance().PollWait(coro::StScheduler::Queued() ? 0 : 1);
	}
#else
	{
		::OI::Thread::MutexGuard lock(st->mu);
		while (!st->ready)
		{
			st->cv.Wait(st->mu);
		}
	}
#endif
	RedisResult out;
	out.reply = st->reply;
	out.status = st->status;
	out.timeout = out.status == RedisStatus::Timeout;
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
		::OI::Thread::MutexGuard lock(self._wait_mu);
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
		self.Settle(expired[i], std::shared_ptr<redisReply>(), RedisStatus::Timeout);
	}
}

void RedisClient::Settle(RedisCall::State* st, std::shared_ptr<redisReply> reply, RedisStatus status)
{
	int expect = 0;
	if (!st->won.compare_exchange_strong(expect, 1, std::memory_order_acq_rel))
	{
		return;
	}
	const bool sync = st->sync;
	{
		::OI::Thread::MutexGuard lock(st->mu);
		st->reply = std::move(reply);
		st->status = status;
		st->ready = true;
		if (sync)
		{
			st->cv.Signal();
		}
	}
	// 同步调用可能在通知后立即释放状态，出锁后不再解引用它。
	if (sync)
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
	if (st->args.empty())
	{
		Settle(st, {}, RedisStatus::InvalidArgument);
		return;
	}
	if (!IsConnect())
	{
		Settle(st, {}, RedisStatus::Disconnected);
		return;
	}
	std::vector<const char*> argv(st->args.size());
	// 已超时的项也占配对位置，不能无限积压。
	if (_wait.size() >= static_cast<size_t>(_max_pending))
	{
		Settle(st, {}, RedisStatus::Busy);
		return;
	}
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
		Settle(st, {}, RedisStatus::InvalidArgument);
		return;
	}
	::OI::Base::Bytes frame;
	frame.Append(cmd, (size_t)n);
	redisFreeCommand(cmd);
	std::call_once(g_sweep_once, []() {
		::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new RedisSweepTimer(), kSweepMs);
	});
	st->deadline_ms = NowMs() + _timeout_ms;
	st->refs.fetch_add(1, std::memory_order_relaxed);
	{
		::OI::Thread::MutexGuard lock(_wait_mu);
		_wait.push_back(st);
	}
	if (!SendRaw(frame))
	{
		::OI::Thread::MutexGuard lock(_wait_mu);
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
		Settle(st, {}, RedisStatus::SendFailed);
		Release(st);
		return;
	}
}

void RedisClient::FailAll(RedisStatus status)
{
	std::deque<RedisCall::State*> left;
	{
		::OI::Thread::MutexGuard lock(_wait_mu);
		left.swap(_wait);
		_swept = 0;
	}
	for (size_t i = 0; i < left.size(); ++i)
	{
		if (left[i]->won.load(std::memory_order_acquire) == 0)
		{
			Settle(left[i], {}, status);
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
		FailAll(RedisStatus::ProtocolError);
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
			FailAll(RedisStatus::ProtocolError);
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
			::OI::Thread::MutexGuard lock(_wait_mu);
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
			const auto status = held->type == REDIS_REPLY_ERROR ? RedisStatus::ServerError : RedisStatus::Ok;
			Settle(st, std::move(held), status);
		}
		Release(st);
	}
}
}}
