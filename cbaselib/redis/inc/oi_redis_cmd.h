#pragma once
#include "oi_redis.h"
#include "oi_duration.h"
namespace OI
{
namespace Redis
{

// 每条 Redis 命令一层薄封装。协程拿到 RedisCall 再 co_await，同步直接返回 RedisResult。

inline RedisCall RedisPing(RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("PING");
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisResult RedisPingSync()
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("PING");
	return CallRedisSync(std::move(vArgumentsLocal));
}

inline RedisCall RedisGet(const std::string& sKeyParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("GET");
	vArgumentsLocal.push_back(sKeyParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisResult RedisGetSync(const std::string& sKeyParam)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("GET");
	vArgumentsLocal.push_back(sKeyParam);
	return CallRedisSync(std::move(vArgumentsLocal));
}

inline RedisCall RedisSet(const std::string& sKeyParam, const std::string& sValueParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("SET");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sValueParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisResult RedisSetSync(const std::string& sKeyParam, const std::string& sValueParam)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("SET");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sValueParam);
	return CallRedisSync(std::move(vArgumentsLocal));
}

inline RedisCall RedisDel(const std::string& sKeyParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("DEL");
	vArgumentsLocal.push_back(sKeyParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisResult RedisDelSync(const std::string& sKeyParam)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("DEL");
	vArgumentsLocal.push_back(sKeyParam);
	return CallRedisSync(std::move(vArgumentsLocal));
}

inline RedisCall RedisIncr(const std::string& sKeyParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("INCR");
	vArgumentsLocal.push_back(sKeyParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisResult RedisIncrSync(const std::string& sKeyParam)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("INCR");
	vArgumentsLocal.push_back(sKeyParam);
	return CallRedisSync(std::move(vArgumentsLocal));
}

inline RedisCall RedisExpire(const std::string& sKeyParam, const std::string& sSecondsParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("EXPIRE");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sSecondsParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisCall RedisHSet(const std::string& sKeyParam, const std::string& sFieldParam, const std::string& sValueParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("HSET");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sFieldParam);
	vArgumentsLocal.push_back(sValueParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisResult RedisHSetSync(const std::string& sKeyParam, const std::string& sFieldParam, const std::string& sValueParam)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("HSET");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sFieldParam);
	vArgumentsLocal.push_back(sValueParam);
	return CallRedisSync(std::move(vArgumentsLocal));
}

inline RedisCall RedisHGet(const std::string& sKeyParam, const std::string& sFieldParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("HGET");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sFieldParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisResult RedisHGetSync(const std::string& sKeyParam, const std::string& sFieldParam)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("HGET");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sFieldParam);
	return CallRedisSync(std::move(vArgumentsLocal));
}

inline RedisCall RedisLPush(const std::string& sKeyParam, const std::string& sValueParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("LPUSH");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sValueParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisCall RedisLTrim(const std::string& sKeyParam, const std::string& sStartParam, const std::string& sStopParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("LTRIM");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sStartParam);
	vArgumentsLocal.push_back(sStopParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisCall RedisLRange(const std::string& sKeyParam, const std::string& sStartParam, const std::string& sStopParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("LRANGE");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sStartParam);
	vArgumentsLocal.push_back(sStopParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisCall RedisSAdd(const std::string& sKeyParam, const std::string& sMemberParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("SADD");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sMemberParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}

inline RedisCall RedisSIsMember(const std::string& sKeyParam, const std::string& sMemberParam, RedisResult* pOutputParam = nullptr)
{
	std::vector<std::string> vArgumentsLocal;
	vArgumentsLocal.push_back("SISMEMBER");
	vArgumentsLocal.push_back(sKeyParam);
	vArgumentsLocal.push_back(sMemberParam);
	return RedisClient::GetInstance().Call(std::move(vArgumentsLocal), pOutputParam);
}
inline RedisCall RedisPing(RedisResult& stOutputParam)
{
	return RedisPing(&stOutputParam);
}

inline RedisCall RedisGet(const std::string& sKeyParam, RedisResult& stOutputParam)
{
	return RedisGet(sKeyParam, &stOutputParam);
}

inline RedisCall RedisSet(const std::string& sKeyParam, const std::string& sValueParam, RedisResult& stOutputParam)
{
	return RedisSet(sKeyParam, sValueParam, &stOutputParam);
}

inline RedisCall RedisDel(const std::string& sKeyParam, RedisResult& stOutputParam)
{
	return RedisDel(sKeyParam, &stOutputParam);
}

inline RedisCall RedisIncr(const std::string& sKeyParam, RedisResult& stOutputParam)
{
	return RedisIncr(sKeyParam, &stOutputParam);
}

inline RedisCall RedisExpire(const std::string& sKeyParam, const std::string& sSecondsParam, RedisResult& stOutputParam)
{
	return RedisExpire(sKeyParam, sSecondsParam, &stOutputParam);
}

inline RedisCall RedisHSet(const std::string& sKeyParam, const std::string& sFieldParam, const std::string& sValueParam, RedisResult& stOutputParam)
{
	return RedisHSet(sKeyParam, sFieldParam, sValueParam, &stOutputParam);
}

inline RedisCall RedisHGet(const std::string& sKeyParam, const std::string& sFieldParam, RedisResult& stOutputParam)
{
	return RedisHGet(sKeyParam, sFieldParam, &stOutputParam);
}

inline RedisCall RedisLPush(const std::string& sKeyParam, const std::string& sValueParam, RedisResult& stOutputParam)
{
	return RedisLPush(sKeyParam, sValueParam, &stOutputParam);
}

inline RedisCall RedisLTrim(const std::string& sKeyParam, const std::string& sStartParam, const std::string& sStopParam, RedisResult& stOutputParam)
{
	return RedisLTrim(sKeyParam, sStartParam, sStopParam, &stOutputParam);
}

inline RedisCall RedisLRange(const std::string& sKeyParam, const std::string& sStartParam, const std::string& sStopParam, RedisResult& stOutputParam)
{
	return RedisLRange(sKeyParam, sStartParam, sStopParam, &stOutputParam);
}

inline RedisCall RedisSAdd(const std::string& sKeyParam, const std::string& sMemberParam, RedisResult& stOutputParam)
{
	return RedisSAdd(sKeyParam, sMemberParam, &stOutputParam);
}

inline RedisCall RedisSIsMember(const std::string& sKeyParam, const std::string& sMemberParam, RedisResult& stOutputParam)
{
	return RedisSIsMember(sKeyParam, sMemberParam, &stOutputParam);
}

inline RedisCall RedisExpire(const std::string& sKeyParam, ::OI::Timer::MS stTimeToLiveParam, RedisResult* pOutputParam = nullptr)
{
	return RedisClient::GetInstance().Call({"PEXPIRE", sKeyParam, std::to_string(stTimeToLiveParam.iMilliseconds)}, pOutputParam);
}

inline RedisCall RedisLTrim(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, RedisResult* pOutputParam = nullptr)
{
	return RedisLTrim(sKeyParam, std::to_string(iStartParam), std::to_string(iStopParam), pOutputParam);
}

inline RedisCall RedisLRange(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, RedisResult* pOutputParam = nullptr)
{
	return RedisLRange(sKeyParam, std::to_string(iStartParam), std::to_string(iStopParam), pOutputParam);
}

}
}
