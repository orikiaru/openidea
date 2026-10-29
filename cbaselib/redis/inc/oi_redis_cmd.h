#pragma once
#include "oi_redis.h"
#include "oi_duration.h"
namespace OI { namespace Redis {

// 每条 Redis 命令一层薄封装。协程拿到 RedisCall 再 co_await，同步直接返回 RedisResult。

inline RedisCall RedisPing(RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("PING");
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisResult RedisPingSync()
{
	std::vector<std::string> args;
	args.push_back("PING");
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisGet(const std::string& key, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("GET");
	args.push_back(key);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisResult RedisGetSync(const std::string& key)
{
	std::vector<std::string> args;
	args.push_back("GET");
	args.push_back(key);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisSet(const std::string& key, const std::string& value, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("SET");
	args.push_back(key);
	args.push_back(value);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisResult RedisSetSync(const std::string& key, const std::string& value)
{
	std::vector<std::string> args;
	args.push_back("SET");
	args.push_back(key);
	args.push_back(value);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisDel(const std::string& key, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("DEL");
	args.push_back(key);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisResult RedisDelSync(const std::string& key)
{
	std::vector<std::string> args;
	args.push_back("DEL");
	args.push_back(key);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisIncr(const std::string& key, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("INCR");
	args.push_back(key);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisResult RedisIncrSync(const std::string& key)
{
	std::vector<std::string> args;
	args.push_back("INCR");
	args.push_back(key);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisExpire(const std::string& key, const std::string& seconds, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("EXPIRE");
	args.push_back(key);
	args.push_back(seconds);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisCall RedisHSet(const std::string& key, const std::string& field, const std::string& value, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("HSET");
	args.push_back(key);
	args.push_back(field);
	args.push_back(value);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisResult RedisHSetSync(const std::string& key, const std::string& field, const std::string& value)
{
	std::vector<std::string> args;
	args.push_back("HSET");
	args.push_back(key);
	args.push_back(field);
	args.push_back(value);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisHGet(const std::string& key, const std::string& field, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("HGET");
	args.push_back(key);
	args.push_back(field);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisResult RedisHGetSync(const std::string& key, const std::string& field)
{
	std::vector<std::string> args;
	args.push_back("HGET");
	args.push_back(key);
	args.push_back(field);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisLPush(const std::string& key, const std::string& value, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("LPUSH");
	args.push_back(key);
	args.push_back(value);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisCall RedisLTrim(const std::string& key, const std::string& start, const std::string& stop, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("LTRIM");
	args.push_back(key);
	args.push_back(start);
	args.push_back(stop);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisCall RedisLRange(const std::string& key, const std::string& start, const std::string& stop, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("LRANGE");
	args.push_back(key);
	args.push_back(start);
	args.push_back(stop);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisCall RedisSAdd(const std::string& key, const std::string& member, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("SADD");
	args.push_back(key);
	args.push_back(member);
	return RedisClient::GetInstance().Call(std::move(args), out);
}

inline RedisCall RedisSIsMember(const std::string& key, const std::string& member, RedisResult* out = nullptr)
{
	std::vector<std::string> args;
	args.push_back("SISMEMBER");
	args.push_back(key);
	args.push_back(member);
	return RedisClient::GetInstance().Call(std::move(args), out);
}
inline RedisCall RedisPing(RedisResult& out)
{
	return RedisPing(&out);
}

inline RedisCall RedisGet(const std::string& key, RedisResult& out)
{
	return RedisGet(key, &out);
}

inline RedisCall RedisSet(const std::string& key, const std::string& value, RedisResult& out)
{
	return RedisSet(key, value, &out);
}

inline RedisCall RedisDel(const std::string& key, RedisResult& out)
{
	return RedisDel(key, &out);
}

inline RedisCall RedisIncr(const std::string& key, RedisResult& out)
{
	return RedisIncr(key, &out);
}

inline RedisCall RedisExpire(const std::string& key, const std::string& seconds, RedisResult& out)
{
	return RedisExpire(key, seconds, &out);
}

inline RedisCall RedisHSet(const std::string& key, const std::string& field, const std::string& value, RedisResult& out)
{
	return RedisHSet(key, field, value, &out);
}

inline RedisCall RedisHGet(const std::string& key, const std::string& field, RedisResult& out)
{
	return RedisHGet(key, field, &out);
}

inline RedisCall RedisLPush(const std::string& key, const std::string& value, RedisResult& out)
{
	return RedisLPush(key, value, &out);
}

inline RedisCall RedisLTrim(const std::string& key, const std::string& start, const std::string& stop, RedisResult& out)
{
	return RedisLTrim(key, start, stop, &out);
}

inline RedisCall RedisLRange(const std::string& key, const std::string& start, const std::string& stop, RedisResult& out)
{
	return RedisLRange(key, start, stop, &out);
}

inline RedisCall RedisSAdd(const std::string& key, const std::string& member, RedisResult& out)
{
	return RedisSAdd(key, member, &out);
}

inline RedisCall RedisSIsMember(const std::string& key, const std::string& member, RedisResult& out)
{
	return RedisSIsMember(key, member, &out);
}

inline RedisCall RedisExpire(const std::string& key, std::chrono::seconds ttl, RedisResult* out = nullptr)
{
	return RedisExpire(key, std::to_string(ttl.count()), out);
}

inline RedisCall RedisLTrim(const std::string& key, int64_t start, int64_t stop, RedisResult* out = nullptr)
{
	return RedisLTrim(key, std::to_string(start), std::to_string(stop), out);
}

inline RedisCall RedisLRange(const std::string& key, int64_t start, int64_t stop, RedisResult* out = nullptr)
{
	return RedisLRange(key, std::to_string(start), std::to_string(stop), out);
}

}}
