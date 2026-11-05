#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisGet(const std::string& sKey, T& stOutput)
{
	return Detail::Read({"GET", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisGetSync(const std::string& sKey, T& stOutput)
{
	return Detail::ReadSync({"GET", sKey}, stOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisDel(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"DEL", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisDelSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"DEL", sKey}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisDel(const std::string& sKey)
{
	return Detail::Status({"DEL", sKey}, REDIS_REPLY_INTEGER);
}

[[nodiscard]] inline int32_t RedisDelSync(const std::string& sKey)
{
	return Detail::StatusSync({"DEL", sKey}, REDIS_REPLY_INTEGER);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisExists(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"EXISTS", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisExistsSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"EXISTS", sKey}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisIncr(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"INCR", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisIncrSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"INCR", sKey}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisDecr(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"DECR", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisDecrSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"DECR", sKey}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisTtl(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"TTL", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisTtlSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"TTL", sKey}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisPTtl(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"PTTL", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisPTtlSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"PTTL", sKey}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSet(const std::string& sKey, const T& stValue)
{
	return Detail::Status({"SET", sKey, stValue}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSetSync(const std::string& sKey, const T& stValue)
{
	return Detail::StatusSync({"SET", sKey, stValue}, REDIS_REPLY_STATUS);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisExpire(const std::string& sKey, OI::Timer::MS stTimeToLive, int64_t& iOutput)
{
	return Detail::Read({"PEXPIRE", sKey, std::to_string(stTimeToLive.iMilliseconds)}, iOutput);
}

[[nodiscard]] inline int32_t RedisExpireSync(const std::string& sKey, OI::Timer::MS stTimeToLive, int64_t& iOutput)
{
	return Detail::ReadSync({"PEXPIRE", sKey, std::to_string(stTimeToLive.iMilliseconds)}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisType(const std::string& sKey, std::string& sOutput)
{
	return Detail::Read({"TYPE", sKey}, sOutput);
}

[[nodiscard]] inline int32_t RedisTypeSync(const std::string& sKey, std::string& sOutput)
{
	return Detail::ReadSync({"TYPE", sKey}, sOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisUnlink(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"UNLINK", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisUnlinkSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"UNLINK", sKey}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisPersist(const std::string& sKey, bool& bOutput)
{
	return Detail::Read({"PERSIST", sKey}, bOutput);
}

[[nodiscard]] inline int32_t RedisPersistSync(const std::string& sKey, bool& bOutput)
{
	return Detail::ReadSync({"PERSIST", sKey}, bOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisStrLen(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"STRLEN", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisStrLenSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"STRLEN", sKey}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisExpireAt(const std::string& sKey, int64_t iUnixSeconds, bool& bOutput)
{
	return Detail::Read({"EXPIREAT", sKey, iUnixSeconds}, bOutput);
}

[[nodiscard]] inline int32_t RedisExpireAtSync(const std::string& sKey, int64_t iUnixSeconds, bool& bOutput)
{
	return Detail::ReadSync({"EXPIREAT", sKey, iUnixSeconds}, bOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisPExpireAt(const std::string& sKey, int64_t iUnixMilliseconds, bool& bOutput)
{
	return Detail::Read({"PEXPIREAT", sKey, iUnixMilliseconds}, bOutput);
}

[[nodiscard]] inline int32_t RedisPExpireAtSync(const std::string& sKey, int64_t iUnixMilliseconds, bool& bOutput)
{
	return Detail::ReadSync({"PEXPIREAT", sKey, iUnixMilliseconds}, bOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisGetDel(const std::string& sKey, T& stOutput)
{
	return Detail::Read({"GETDEL", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisGetDelSync(const std::string& sKey, T& stOutput)
{
	return Detail::ReadSync({"GETDEL", sKey}, stOutput);
}

template <typename T, typename U>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisGetSet(const std::string& sKey, const T& stValue, U& stOutput)
{
	return Detail::Read({"GETSET", sKey, stValue}, stOutput);
}

template <typename T, typename U>
[[nodiscard]] inline int32_t RedisGetSetSync(const std::string& sKey, const T& stValue, U& stOutput)
{
	return Detail::ReadSync({"GETSET", sKey, stValue}, stOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSetNx(const std::string& sKey, const T& stValue, bool& bOutput)
{
	return Detail::Read({"SETNX", sKey, stValue}, bOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSetNxSync(const std::string& sKey, const T& stValue, bool& bOutput)
{
	return Detail::ReadSync({"SETNX", sKey, stValue}, bOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSet(const std::string& sKey, const T& stValue, OI::Timer::MS stTimeToLive)
{
	return Detail::Status({"SET", sKey, stValue, "PX", std::to_string(stTimeToLive.iMilliseconds)}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSetSync(const std::string& sKey, const T& stValue, OI::Timer::MS stTimeToLive)
{
	return Detail::StatusSync({"SET", sKey, stValue, "PX", std::to_string(stTimeToLive.iMilliseconds)}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisGetEx(const std::string& sKey, OI::Timer::MS stTimeToLive, T& stOutput)
{
	return Detail::Read({"GETEX", sKey, "PX", std::to_string(stTimeToLive.iMilliseconds)}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisGetExSync(const std::string& sKey, OI::Timer::MS stTimeToLive, T& stOutput)
{
	return Detail::ReadSync({"GETEX", sKey, "PX", std::to_string(stTimeToLive.iMilliseconds)}, stOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisGetPersist(const std::string& sKey, T& stOutput)
{
	return Detail::Read({"GETEX", sKey, "PERSIST"}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisGetPersistSync(const std::string& sKey, T& stOutput)
{
	return Detail::ReadSync({"GETEX", sKey, "PERSIST"}, stOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisAppend(const std::string& sKey, const std::string& sValue, int64_t& iOutput)
{
	return Detail::Read({"APPEND", sKey, sValue}, iOutput);
}

[[nodiscard]] inline int32_t RedisAppendSync(const std::string& sKey, const std::string& sValue, int64_t& iOutput)
{
	return Detail::ReadSync({"APPEND", sKey, sValue}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisGetRange(const std::string& sKey, int64_t iStart, int64_t iStop, std::string& sOutput)
{
	return Detail::Read({"GETRANGE", sKey, iStart, iStop}, sOutput);
}

[[nodiscard]] inline int32_t RedisGetRangeSync(const std::string& sKey, int64_t iStart, int64_t iStop, std::string& sOutput)
{
	return Detail::ReadSync({"GETRANGE", sKey, iStart, iStop}, sOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSetRange(const std::string& sKey, int64_t iOffset, const std::string& sValue, int64_t& iOutput)
{
	return Detail::Read({"SETRANGE", sKey, iOffset, sValue}, iOutput);
}

[[nodiscard]] inline int32_t RedisSetRangeSync(const std::string& sKey, int64_t iOffset, const std::string& sValue, int64_t& iOutput)
{
	return Detail::ReadSync({"SETRANGE", sKey, iOffset, sValue}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisIncrBy(const std::string& sKey, int64_t iDelta, int64_t& iOutput)
{
	return Detail::Read({"INCRBY", sKey, iDelta}, iOutput);
}

[[nodiscard]] inline int32_t RedisIncrBySync(const std::string& sKey, int64_t iDelta, int64_t& iOutput)
{
	return Detail::ReadSync({"INCRBY", sKey, iDelta}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisDecrBy(const std::string& sKey, int64_t iDelta, int64_t& iOutput)
{
	return Detail::Read({"DECRBY", sKey, iDelta}, iOutput);
}

[[nodiscard]] inline int32_t RedisDecrBySync(const std::string& sKey, int64_t iDelta, int64_t& iOutput)
{
	return Detail::ReadSync({"DECRBY", sKey, iDelta}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisIncrByFloat(const std::string& sKey, double dDelta, double& dOutput)
{
	return Detail::Read({"INCRBYFLOAT", sKey, dDelta}, dOutput);
}

[[nodiscard]] inline int32_t RedisIncrByFloatSync(const std::string& sKey, double dDelta, double& dOutput)
{
	return Detail::ReadSync({"INCRBYFLOAT", sKey, dDelta}, dOutput);
}
}
}
