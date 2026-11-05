#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisGet(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::Read({"GET", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisGetSync(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::ReadSync({"GET", sKeyParam}, stOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisDel(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"DEL", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisDelSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"DEL", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisDel(const std::string& sKeyParam)
{
	return Detail::Status({"DEL", sKeyParam}, REDIS_REPLY_INTEGER);
}

[[nodiscard]] inline int32_t RedisDelSync(const std::string& sKeyParam)
{
	return Detail::StatusSync({"DEL", sKeyParam}, REDIS_REPLY_INTEGER);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisExists(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"EXISTS", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisExistsSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"EXISTS", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisIncr(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"INCR", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisIncrSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"INCR", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisDecr(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"DECR", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisDecrSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"DECR", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisTtl(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"TTL", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisTtlSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"TTL", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisPTtl(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"PTTL", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisPTtlSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"PTTL", sKeyParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSet(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::Status({"SET", sKeyParam, stValueParam}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSetSync(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::StatusSync({"SET", sKeyParam, stValueParam}, REDIS_REPLY_STATUS);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisExpire(const std::string& sKeyParam, OI::Timer::MS stTimeToLiveParam, int64_t& iOutputParam)
{
	return Detail::Read({"PEXPIRE", sKeyParam, std::to_string(stTimeToLiveParam.iMilliseconds)}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisExpireSync(const std::string& sKeyParam, OI::Timer::MS stTimeToLiveParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"PEXPIRE", sKeyParam, std::to_string(stTimeToLiveParam.iMilliseconds)}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisType(const std::string& sKeyParam, std::string& sOutputParam)
{
	return Detail::Read({"TYPE", sKeyParam}, sOutputParam);
}

[[nodiscard]] inline int32_t RedisTypeSync(const std::string& sKeyParam, std::string& sOutputParam)
{
	return Detail::ReadSync({"TYPE", sKeyParam}, sOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisUnlink(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"UNLINK", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisUnlinkSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"UNLINK", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisPersist(const std::string& sKeyParam, bool& bOutputParam)
{
	return Detail::Read({"PERSIST", sKeyParam}, bOutputParam);
}

[[nodiscard]] inline int32_t RedisPersistSync(const std::string& sKeyParam, bool& bOutputParam)
{
	return Detail::ReadSync({"PERSIST", sKeyParam}, bOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisStrLen(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"STRLEN", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisStrLenSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"STRLEN", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisExpireAt(const std::string& sKeyParam, int64_t iUnixSecondsParam, bool& bOutputParam)
{
	return Detail::Read({"EXPIREAT", sKeyParam, iUnixSecondsParam}, bOutputParam);
}

[[nodiscard]] inline int32_t RedisExpireAtSync(const std::string& sKeyParam, int64_t iUnixSecondsParam, bool& bOutputParam)
{
	return Detail::ReadSync({"EXPIREAT", sKeyParam, iUnixSecondsParam}, bOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisPExpireAt(const std::string& sKeyParam, int64_t iUnixMillisecondsParam, bool& bOutputParam)
{
	return Detail::Read({"PEXPIREAT", sKeyParam, iUnixMillisecondsParam}, bOutputParam);
}

[[nodiscard]] inline int32_t RedisPExpireAtSync(const std::string& sKeyParam, int64_t iUnixMillisecondsParam, bool& bOutputParam)
{
	return Detail::ReadSync({"PEXPIREAT", sKeyParam, iUnixMillisecondsParam}, bOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisGetDel(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::Read({"GETDEL", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisGetDelSync(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::ReadSync({"GETDEL", sKeyParam}, stOutputParam);
}

template <typename T, typename U>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisGetSet(const std::string& sKeyParam, const T& stValueParam, U& stOutputParam)
{
	return Detail::Read({"GETSET", sKeyParam, stValueParam}, stOutputParam);
}

template <typename T, typename U>
[[nodiscard]] inline int32_t RedisGetSetSync(const std::string& sKeyParam, const T& stValueParam, U& stOutputParam)
{
	return Detail::ReadSync({"GETSET", sKeyParam, stValueParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSetNx(const std::string& sKeyParam, const T& stValueParam, bool& bOutputParam)
{
	return Detail::Read({"SETNX", sKeyParam, stValueParam}, bOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSetNxSync(const std::string& sKeyParam, const T& stValueParam, bool& bOutputParam)
{
	return Detail::ReadSync({"SETNX", sKeyParam, stValueParam}, bOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSet(const std::string& sKeyParam, const T& stValueParam, OI::Timer::MS stTimeToLiveParam)
{
	return Detail::Status({"SET", sKeyParam, stValueParam, "PX", std::to_string(stTimeToLiveParam.iMilliseconds)}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSetSync(const std::string& sKeyParam, const T& stValueParam, OI::Timer::MS stTimeToLiveParam)
{
	return Detail::StatusSync({"SET", sKeyParam, stValueParam, "PX", std::to_string(stTimeToLiveParam.iMilliseconds)}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisGetEx(const std::string& sKeyParam, OI::Timer::MS stTimeToLiveParam, T& stOutputParam)
{
	return Detail::Read({"GETEX", sKeyParam, "PX", std::to_string(stTimeToLiveParam.iMilliseconds)}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisGetExSync(const std::string& sKeyParam, OI::Timer::MS stTimeToLiveParam, T& stOutputParam)
{
	return Detail::ReadSync({"GETEX", sKeyParam, "PX", std::to_string(stTimeToLiveParam.iMilliseconds)}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisGetPersist(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::Read({"GETEX", sKeyParam, "PERSIST"}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisGetPersistSync(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::ReadSync({"GETEX", sKeyParam, "PERSIST"}, stOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisAppend(const std::string& sKeyParam, const std::string& sValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"APPEND", sKeyParam, sValueParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisAppendSync(const std::string& sKeyParam, const std::string& sValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"APPEND", sKeyParam, sValueParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisGetRange(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::string& sOutputParam)
{
	return Detail::Read({"GETRANGE", sKeyParam, iStartParam, iStopParam}, sOutputParam);
}

[[nodiscard]] inline int32_t RedisGetRangeSync(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::string& sOutputParam)
{
	return Detail::ReadSync({"GETRANGE", sKeyParam, iStartParam, iStopParam}, sOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisSetRange(const std::string& sKeyParam, int64_t iOffsetParam, const std::string& sValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"SETRANGE", sKeyParam, iOffsetParam, sValueParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisSetRangeSync(const std::string& sKeyParam, int64_t iOffsetParam, const std::string& sValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"SETRANGE", sKeyParam, iOffsetParam, sValueParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisIncrBy(const std::string& sKeyParam, int64_t iDeltaParam, int64_t& iOutputParam)
{
	return Detail::Read({"INCRBY", sKeyParam, iDeltaParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisIncrBySync(const std::string& sKeyParam, int64_t iDeltaParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"INCRBY", sKeyParam, iDeltaParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisDecrBy(const std::string& sKeyParam, int64_t iDeltaParam, int64_t& iOutputParam)
{
	return Detail::Read({"DECRBY", sKeyParam, iDeltaParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisDecrBySync(const std::string& sKeyParam, int64_t iDeltaParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"DECRBY", sKeyParam, iDeltaParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisIncrByFloat(const std::string& sKeyParam, double dDeltaParam, double& dOutputParam)
{
	return Detail::Read({"INCRBYFLOAT", sKeyParam, dDeltaParam}, dOutputParam);
}

[[nodiscard]] inline int32_t RedisIncrByFloatSync(const std::string& sKeyParam, double dDeltaParam, double& dOutputParam)
{
	return Detail::ReadSync({"INCRBYFLOAT", sKeyParam, dDeltaParam}, dOutputParam);
}
}
}
