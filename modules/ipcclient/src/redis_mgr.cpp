#include "redis_mgr.h"
#include "oi_redis.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_config.h"
#include <stdexcept>
#include "test_board.h"
#include <cstring>
#include <string>
#include <thread>
#include <vector>

RedisMgr::RedisMgr()
	: _on(false), _tasks(8), _sync(2), _seconds(10), _timeout_ms(25000), _failed(0), _done(0), _ops(0), _bad(0)
{
}

void RedisMgr::Load()
{
	TryGetConfig("Test.redis", _on);
	TryGetConfig("TestRedis.tasks", _tasks);
	TryGetConfig("TestRedis.seconds", _seconds);
	TryGetConfig("TestRedis.timeout_ms", _timeout_ms);
}

void RedisMgr::Start()
{
	if (!_on)
	{
		return;
	}
	_start = std::chrono::steady_clock::now();
	TestBoard::GetInstance().Arm();
	coro::StartTask([](coro::Token) {
		return RedisMgr::GetInstance().Pump();
	});
}

static bool StatusIs(const ::OI::Redis::RedisResult& result, const char* text)
{
	return !result.timeout && result.reply && result.reply->type == REDIS_REPLY_STATUS
		&& result.reply->str && std::strcmp(result.reply->str, text) == 0;
}

coro::Task RedisMgr::Pump()
{
	int spins = 0;
	int limit = _timeout_ms > 0 ? _timeout_ms / 50 : 100;
	if (limit < 1)
	{
		limit = 1;
	}
	while (!::OI::Redis::RedisClient::GetInstance().IsConnect())
	{
		if (spins++ >= limit)
		{
			LOG_TRACE("redis connect timeout");
			TestBoard::GetInstance().Done(1);
			co_return;
		}
		co_await coro::yield(50);
	}

	_failed = 0;
	co_await Check();
	if (_failed)
	{
		LOG_TRACE("redis integrity failed=%d", _failed);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	LOG_TRACE("redis integrity ok");
	_start = std::chrono::steady_clock::now();
	if (_tasks <= 0 || _seconds <= 0)
	{
		TestBoard::GetInstance().Done(0);
		co_return;
	}
	_done.store(0);
	for (int i = 0; i < _tasks; ++i)
	{
		coro::StartTask([i](coro::Token) {
			return RedisMgr::GetInstance().Stress(i);
		});
	}
#ifdef _SINGLE_THREAD_
	// 收包只有这一条线程。另开线程再 join，poll 被堵住，同步调用永远等不到回包。
	for (int i = 0; i < _sync; ++i)
	{
		SyncStress(i);
	}
#else
	std::vector<std::thread> syncs;
	for (int i = 0; i < _sync; ++i)
	{
		syncs.emplace_back(&RedisMgr::SyncStress, this, i);
	}
	for (size_t i = 0; i < syncs.size(); ++i)
	{
		syncs[i].join();
	}
#endif
	co_return;
}

static bool IntIs(const ::OI::Redis::RedisResult& result, long long n)
{
	return !result.timeout && result.reply && result.reply->type == REDIS_REPLY_INTEGER && result.reply->integer == n;
}

static bool BulkIs(const ::OI::Redis::RedisResult& result, const char* text, size_t n)
{
	return !result.timeout && result.reply && result.reply->type == REDIS_REPLY_STRING
		&& result.reply->len == n && result.reply->str && std::memcmp(result.reply->str, text, n) == 0;
}

coro::Task RedisMgr::Check()
{
	int32_t pending_limit = 65536;
	TryGetConfig("RedisClient.max_pending", pending_limit);
	if (pending_limit == 1)
	{
		struct Pending { std::atomic<bool> done{false}; ::OI::Redis::RedisResult result; };
		auto pending = std::make_shared<Pending>();
		coro::StartTask([pending](coro::Token) -> coro::Task {
			pending->result = co_await ::OI::Redis::CallRedis({"BLPOP", "oi-limit-empty", "1"});
			pending->done.store(true, std::memory_order_release);
		});
		while (::OI::Redis::RedisClient::GetInstance().PendingCount() == 0 && !pending->done.load(std::memory_order_acquire))
			co_await coro::yield(::OI::Timer::Milliseconds(10));
		auto busy = co_await ::OI::Redis::RedisPing();
		if (busy.status != ::OI::Redis::RedisStatus::Busy || busy.timeout) ++_failed;
		while (!pending->done.load(std::memory_order_acquire)) co_await coro::yield(::OI::Timer::Milliseconds(10));
		if (!pending->result || !pending->result.reply || pending->result.reply->type != REDIS_REPLY_NIL) ++_failed;
		auto recovered = co_await ::OI::Redis::RedisPing();
		if (!StatusIs(recovered, "PONG")) ++_failed;
		LOG_TRACE("redis pending limit bad=%d", _failed);
		co_return;
	}
	auto invalid = co_await ::OI::Redis::CallRedis({});
	if (invalid.status != ::OI::Redis::RedisStatus::InvalidArgument || invalid.timeout) ++_failed;
	auto direct = co_await ::OI::Redis::RedisPing();
	if (!direct || !StatusIs(direct, "PONG")) ++_failed;
	auto error = co_await ::OI::Redis::CallRedis({"OI_UNKNOWN_COMMAND"});
	if (error.status != ::OI::Redis::RedisStatus::ServerError || !error.reply || error.timeout) ++_failed;

#ifndef _SINGLE_THREAD_
	std::atomic<int32_t> sync_guard(0);
	::OI::Net::Poll::GetInstance().CallOnLoop([](void* arg) {
		int32_t checked = -1;
		try { ::OI::Redis::RedisPingSync(); }
		catch (const std::logic_error&) { checked = 1; }
		((std::atomic<int32_t>*)arg)->store(checked, std::memory_order_release);
	}, &sync_guard);
	while (sync_guard.load(std::memory_order_acquire) == 0) co_await coro::yield(10);
	if (sync_guard.load() != 1) ++_failed;
#endif
	::OI::Redis::RedisResult synced = ::OI::Redis::RedisPingSync();
	if (!StatusIs(synced, "PONG"))
	{
		++_failed;
	}
	// 计数和新建字段的断言依赖空键。上一轮留下的值会让 HSET 返回 0、INCR 从中间开始。
	auto wiped = co_await ::OI::Redis::CallRedis({"DEL", "rk", "rbin", "rk-missing", "rk-exp", "rh", "rl", "rs", "rk-count"});
	if (wiped.timeout || !wiped.reply || wiped.reply->type != REDIS_REPLY_INTEGER) ++_failed;
	::OI::Redis::RedisResult set;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisSet("rk", "hello", set);
		co_await call;
	}
	if (!StatusIs(set, "OK"))
	{
		++_failed;
	}
	::OI::Redis::RedisResult get;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisGet("rk", get);
		co_await call;
	}
	if (!BulkIs(get, "hello", 5))
	{
		++_failed;
	}
	std::string bin("a\0b", 3);
	::OI::Redis::RedisResult setb;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisSet("rbin", bin, setb);
		co_await call;
	}
	if (!StatusIs(setb, "OK"))
	{
		++_failed;
	}
	::OI::Redis::RedisResult getb;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisGet("rbin", getb);
		co_await call;
	}
	if (!BulkIs(getb, bin.data(), 3))
	{
		++_failed;
	}
	::OI::Redis::RedisResult miss;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisGet("rk-missing", miss);
		co_await call;
	}
	if (miss.timeout || !miss.reply || miss.reply->type != REDIS_REPLY_NIL)
	{
		++_failed;
	}
	::OI::Redis::RedisResult gone;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisDel("rk", gone);
		co_await call;
	}
	if (!IntIs(gone, 1))
	{
		++_failed;
	}
	::OI::Redis::RedisResult exp;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisSet("rk-exp", "1", exp);
		co_await call;
	}
	auto ttl = co_await ::OI::Redis::RedisExpire("rk-exp", ::OI::Timer::Seconds(60));
	if (!IntIs(ttl, 1))
	{
		++_failed;
	}
	::OI::Redis::RedisResult hset;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisHSet("rh", "f", "v", hset);
		co_await call;
	}
	if (!IntIs(hset, 1))
	{
		++_failed;
	}
	::OI::Redis::RedisResult hget;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisHGet("rh", "f", hget);
		co_await call;
	}
	if (!BulkIs(hget, "v", 1))
	{
		++_failed;
	}
	::OI::Redis::RedisResult lpush;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisLPush("rl", "a", lpush);
		co_await call;
	}
	::OI::Redis::RedisResult lpush2;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisLPush("rl", "b", lpush2);
		co_await call;
	}
	auto lrange = co_await ::OI::Redis::RedisLRange("rl", 0, 1);
	if (lrange.timeout || !lrange.reply || lrange.reply->type != REDIS_REPLY_ARRAY || lrange.reply->elements != 2)
	{
		++_failed;
	}
	::OI::Redis::RedisResult sadd;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisSAdd("rs", "m", sadd);
		co_await call;
	}
	::OI::Redis::RedisResult sismember;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::RedisSIsMember("rs", "m", sismember);
		co_await call;
	}
	if (!IntIs(sismember, 1))
	{
		++_failed;
	}
	for (int i = 1; i <= 20; ++i)
	{
		::OI::Redis::RedisResult n;
		::OI::Redis::RedisCall call = ::OI::Redis::RedisIncr("rk-count", n);
		co_await call;
		if (!IntIs(n, i))
		{
			++_failed;
			break;
		}
	}
	// 状态命令必须被代理拒绝；重连后普通命令仍能使用，不能留下事务或切库状态。
	const char* denied[] = {"SELECT", "MULTI", "CLIENT", "SUBSCRIBE"};
	for (const char* command : denied)
	{
		::OI::Redis::RedisResult rejected;
		::OI::Redis::RedisCall call = ::OI::Redis::CallRedis({command, "1"}, rejected);
		co_await call;
		if (rejected.status != ::OI::Redis::RedisStatus::Disconnected || rejected.timeout) ++_failed;
		int attempts = 0;
		while (!::OI::Redis::RedisClient::GetInstance().IsConnect() && attempts++ < 100)
		{
			co_await coro::yield(50);
		}
		::OI::Redis::RedisResult pong;
		::OI::Redis::RedisCall check = ::OI::Redis::RedisPing(pong);
		co_await check;
		if (!StatusIs(pong, "PONG")) ++_failed;
	}
	co_return;
}

void RedisMgr::NoteDone()
{
	int total = _tasks + _sync;
	if (total < 1)
	{
		total = 1;
	}
	if (_done.fetch_add(1) + 1 != total)
	{
		return;
	}
	long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - _start).count();
	if (ms < 1)
	{
		ms = 1;
	}
	long long ops = _ops.load();
	int bad = _bad.load();
	LOG_TRACE("redis stress tasks=%d sync=%d ops=%lld ms=%lld rate=%lld/s bad=%d",
		_tasks, _sync, ops, ms, ops * 1000 / ms, bad);
	TestBoard::GetInstance().Done(bad == 0 ? 0 : 1);
}

void RedisMgr::SyncStress(int id)
{
	std::string key = "sync:" + std::to_string(id);
	auto end = _start + std::chrono::seconds(_seconds);
	while (std::chrono::steady_clock::now() < end)
	{
		::OI::Redis::RedisResult put = ::OI::Redis::RedisSetSync(key, "s");
		::OI::Redis::RedisResult got = ::OI::Redis::RedisGetSync(key);
		if (!StatusIs(put, "OK") || !BulkIs(got, "s", 1))
		{
			_bad.fetch_add(1);
		}
		else
		{
			_ops.fetch_add(2);
		}
	}
	NoteDone();
}

coro::Task RedisMgr::Stress(int id)
{
	std::string value(64, 'x');
	std::string hash = "h:" + std::to_string(id);
	std::string list = "l:" + std::to_string(id);
	auto end = _start + std::chrono::seconds(_seconds);
	int seq = 0;
	int kind = id % 4;
	while (std::chrono::steady_clock::now() < end)
	{
		std::string key = "k:" + std::to_string(id) + ":" + std::to_string(seq % 256);
		std::string member = std::to_string(seq % 32);
		++seq;
		::OI::Redis::RedisResult result;
		bool ok = false;
		if (kind == 0)
		{
			result = co_await ::OI::Redis::RedisSet(key, value);
			ok = StatusIs(result, "OK");
		}
		else if (kind == 1)
		{
			result = co_await ::OI::Redis::RedisHSet(hash, member, value);
			ok = IntIs(result, 0) || IntIs(result, 1);
		}
		else if (kind == 2)
		{
			result = co_await ::OI::Redis::RedisLPush(list, member);
			ok = !result.timeout && result.reply && result.reply->type == REDIS_REPLY_INTEGER;
			if (ok && (seq % 16) == 0)
			{
				auto trim = co_await ::OI::Redis::RedisLTrim(list, 0, 15);
				if (!StatusIs(trim, "OK"))
				{
					ok = false;
				}
			}
		}
		else
		{
			result = co_await ::OI::Redis::RedisIncr(key);
			ok = !result.timeout && result.reply && result.reply->type == REDIS_REPLY_INTEGER;
		}
		if (!ok)
		{
			_bad.fetch_add(1);
		}
		else
		{
			_ops.fetch_add(1);
		}
	}
	NoteDone();
	co_return;
}
