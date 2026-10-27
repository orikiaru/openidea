#include "redis_mgr.h"
#include "redis_client.h"
#include "logger.h"
#include "test_board.h"
#include <cstring>
#include <string>
#include <thread>
#include <vector>

RedisMgr::RedisMgr()
	: _on(false), _tasks(8), _sync(2), _seconds(10), _timeout_ms(25000), _failed(0), _done(0), _ops(0), _bad(0)
{
}

RedisMgr& RedisMgr::Get()
{
	static RedisMgr inst;
	return inst;
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
	GetTestBoard().Arm();
	coro::StartTask([](coro::Token) {
		return RedisMgr::Get().Pump();
	});
}

static bool StatusIs(const RedisResult& result, const char* text)
{
	return !result.timeout && result.reply && result.reply->type == REDIS_REPLY_STATUS
		&& result.reply->str && std::strcmp(result.reply->str, text) == 0;
}

coro::Task RedisMgr::Pump()
{
	co_await coro::YieldToPool{};
	int spins = 0;
	int limit = _timeout_ms > 0 ? _timeout_ms / 50 : 100;
	if (limit < 1)
	{
		limit = 1;
	}
	while (!RedisClient::GetInstance().IsConnect())
	{
		if (spins++ >= limit)
		{
			LOG_TRACE("redis connect timeout");
			GetTestBoard().Done(1);
			co_return;
		}
		co_await coro::yield(50);
	}

	_failed = 0;
	co_await Check();
	if (_failed)
	{
		LOG_TRACE("redis integrity failed=%d", _failed);
		GetTestBoard().Done(1);
		co_return;
	}
	LOG_TRACE("redis integrity ok");
	_start = std::chrono::steady_clock::now();
	if (_tasks <= 0 || _seconds <= 0)
	{
		GetTestBoard().Done(0);
		co_return;
	}
	_done.store(0);
	for (int i = 0; i < _tasks; ++i)
	{
		coro::StartTask([i](coro::Token) {
			return RedisMgr::Get().Stress(i);
		});
	}
	std::vector<std::thread> syncs;
	for (int i = 0; i < _sync; ++i)
	{
		syncs.emplace_back(&RedisMgr::SyncStress, this, i);
	}
	for (size_t i = 0; i < syncs.size(); ++i)
	{
		syncs[i].join();
	}
	co_return;
}

static bool IntIs(const RedisResult& result, long long n)
{
	return !result.timeout && result.reply && result.reply->type == REDIS_REPLY_INTEGER && result.reply->integer == n;
}

static bool BulkIs(const RedisResult& result, const char* text, size_t n)
{
	return !result.timeout && result.reply && result.reply->type == REDIS_REPLY_STRING
		&& result.reply->len == n && result.reply->str && std::memcmp(result.reply->str, text, n) == 0;
}

coro::Task RedisMgr::Check()
{
	RedisResult synced = RedisPingSync();
	if (!StatusIs(synced, "PONG"))
	{
		++_failed;
	}
	RedisResult set;
	{
		RedisCall call = RedisSet("rk", "hello", set);
		co_await call;
	}
	if (!StatusIs(set, "OK"))
	{
		++_failed;
	}
	RedisResult get;
	{
		RedisCall call = RedisGet("rk", get);
		co_await call;
	}
	if (!BulkIs(get, "hello", 5))
	{
		++_failed;
	}
	std::string bin("a\0b", 3);
	RedisResult setb;
	{
		RedisCall call = RedisSet("rbin", bin, setb);
		co_await call;
	}
	if (!StatusIs(setb, "OK"))
	{
		++_failed;
	}
	RedisResult getb;
	{
		RedisCall call = RedisGet("rbin", getb);
		co_await call;
	}
	if (!BulkIs(getb, bin.data(), 3))
	{
		++_failed;
	}
	RedisResult miss;
	{
		RedisCall call = RedisGet("rk-missing", miss);
		co_await call;
	}
	if (miss.timeout || !miss.reply || miss.reply->type != REDIS_REPLY_NIL)
	{
		++_failed;
	}
	RedisResult gone;
	{
		RedisCall call = RedisDel("rk", gone);
		co_await call;
	}
	if (!IntIs(gone, 1))
	{
		++_failed;
	}
	RedisResult exp;
	{
		RedisCall call = RedisSet("rk-exp", "1", exp);
		co_await call;
	}
	RedisResult ttl;
	{
		RedisCall call = RedisExpire("rk-exp", "60", ttl);
		co_await call;
	}
	if (!IntIs(ttl, 1))
	{
		++_failed;
	}
	RedisResult hset;
	{
		RedisCall call = RedisHSet("rh", "f", "v", hset);
		co_await call;
	}
	if (!IntIs(hset, 1))
	{
		++_failed;
	}
	RedisResult hget;
	{
		RedisCall call = RedisHGet("rh", "f", hget);
		co_await call;
	}
	if (!BulkIs(hget, "v", 1))
	{
		++_failed;
	}
	RedisResult lpush;
	{
		RedisCall call = RedisLPush("rl", "a", lpush);
		co_await call;
	}
	RedisResult lpush2;
	{
		RedisCall call = RedisLPush("rl", "b", lpush2);
		co_await call;
	}
	RedisResult lrange;
	{
		RedisCall call = RedisLRange("rl", "0", "1", lrange);
		co_await call;
	}
	if (lrange.timeout || !lrange.reply || lrange.reply->type != REDIS_REPLY_ARRAY || lrange.reply->elements != 2)
	{
		++_failed;
	}
	RedisResult sadd;
	{
		RedisCall call = RedisSAdd("rs", "m", sadd);
		co_await call;
	}
	RedisResult sismember;
	{
		RedisCall call = RedisSIsMember("rs", "m", sismember);
		co_await call;
	}
	if (!IntIs(sismember, 1))
	{
		++_failed;
	}
	for (int i = 1; i <= 20; ++i)
	{
		RedisResult n;
		RedisCall call = RedisIncr("rk-count", n);
		co_await call;
		if (!IntIs(n, i))
		{
			++_failed;
			break;
		}
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
	GetTestBoard().Done(bad == 0 ? 0 : 1);
}

void RedisMgr::SyncStress(int id)
{
	std::string key = "sync:" + std::to_string(id);
	auto end = _start + std::chrono::seconds(_seconds);
	while (std::chrono::steady_clock::now() < end)
	{
		RedisResult put = RedisSetSync(key, "s");
		RedisResult got = RedisGetSync(key);
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
	co_await coro::YieldToPool{};
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
		RedisResult result;
		bool ok = false;
		if (kind == 0)
		{
			RedisCall call = RedisSet(key, value, result);
			co_await call;
			ok = StatusIs(result, "OK");
		}
		else if (kind == 1)
		{
			RedisCall call = RedisHSet(hash, member, value, result);
			co_await call;
			ok = IntIs(result, 0) || IntIs(result, 1);
		}
		else if (kind == 2)
		{
			RedisCall call = RedisLPush(list, member, result);
			co_await call;
			ok = !result.timeout && result.reply && result.reply->type == REDIS_REPLY_INTEGER;
			if (ok && (seq % 16) == 0)
			{
				RedisResult trim;
				RedisCall trim_call = RedisLTrim(list, "0", "15", trim);
				co_await trim_call;
				if (!StatusIs(trim, "OK"))
				{
					ok = false;
				}
			}
		}
		else
		{
			RedisCall call = RedisIncr(key, result);
			co_await call;
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
