#pragma once
#include "oi_redis.h"

// 每条 Redis 命令一层薄封装。协程拿到 RedisCall 再 co_await，同步直接返回 RedisResult。

inline RedisCall RedisPing(RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("PING");
	return CallRedis(std::move(args), out);
}

inline RedisResult RedisPingSync()
{
	std::vector<std::string> args;
	args.push_back("PING");
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisGet(const std::string& key, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("GET");
	args.push_back(key);
	return CallRedis(std::move(args), out);
}

inline RedisResult RedisGetSync(const std::string& key)
{
	std::vector<std::string> args;
	args.push_back("GET");
	args.push_back(key);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisSet(const std::string& key, const std::string& value, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("SET");
	args.push_back(key);
	args.push_back(value);
	return CallRedis(std::move(args), out);
}

inline RedisResult RedisSetSync(const std::string& key, const std::string& value)
{
	std::vector<std::string> args;
	args.push_back("SET");
	args.push_back(key);
	args.push_back(value);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisDel(const std::string& key, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("DEL");
	args.push_back(key);
	return CallRedis(std::move(args), out);
}

inline RedisResult RedisDelSync(const std::string& key)
{
	std::vector<std::string> args;
	args.push_back("DEL");
	args.push_back(key);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisIncr(const std::string& key, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("INCR");
	args.push_back(key);
	return CallRedis(std::move(args), out);
}

inline RedisResult RedisIncrSync(const std::string& key)
{
	std::vector<std::string> args;
	args.push_back("INCR");
	args.push_back(key);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisExpire(const std::string& key, const std::string& seconds, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("EXPIRE");
	args.push_back(key);
	args.push_back(seconds);
	return CallRedis(std::move(args), out);
}

inline RedisCall RedisHSet(const std::string& key, const std::string& field, const std::string& value, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("HSET");
	args.push_back(key);
	args.push_back(field);
	args.push_back(value);
	return CallRedis(std::move(args), out);
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

inline RedisCall RedisHGet(const std::string& key, const std::string& field, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("HGET");
	args.push_back(key);
	args.push_back(field);
	return CallRedis(std::move(args), out);
}

inline RedisResult RedisHGetSync(const std::string& key, const std::string& field)
{
	std::vector<std::string> args;
	args.push_back("HGET");
	args.push_back(key);
	args.push_back(field);
	return CallRedisSync(std::move(args));
}

inline RedisCall RedisLPush(const std::string& key, const std::string& value, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("LPUSH");
	args.push_back(key);
	args.push_back(value);
	return CallRedis(std::move(args), out);
}

inline RedisCall RedisLTrim(const std::string& key, const std::string& start, const std::string& stop, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("LTRIM");
	args.push_back(key);
	args.push_back(start);
	args.push_back(stop);
	return CallRedis(std::move(args), out);
}

inline RedisCall RedisLRange(const std::string& key, const std::string& start, const std::string& stop, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("LRANGE");
	args.push_back(key);
	args.push_back(start);
	args.push_back(stop);
	return CallRedis(std::move(args), out);
}

inline RedisCall RedisSAdd(const std::string& key, const std::string& member, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("SADD");
	args.push_back(key);
	args.push_back(member);
	return CallRedis(std::move(args), out);
}

inline RedisCall RedisSIsMember(const std::string& key, const std::string& member, RedisResult& out)
{
	std::vector<std::string> args;
	args.push_back("SISMEMBER");
	args.push_back(key);
	args.push_back(member);
	return CallRedis(std::move(args), out);
}
