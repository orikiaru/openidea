#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHLen(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"HLEN", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisHLenSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"HLEN", sKey}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHGet(const std::string& sKey, const std::string& sField, T& stOutput)
{
	return Detail::Read({"HGET", sKey, sField}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHGetSync(const std::string& sKey, const std::string& sField, T& stOutput)
{
	return Detail::ReadSync({"HGET", sKey, sField}, stOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHGetAll(const std::string& sKey, std::map<std::string, T>& mOutput)
{
	return Detail::Read({"HGETALL", sKey}, mOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHGetAllSync(const std::string& sKey, std::map<std::string, T>& mOutput)
{
	return Detail::ReadSync({"HGETALL", sKey}, mOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHDel(const std::string& sKey, const std::string& sField, int64_t& iOutput)
{
	return Detail::Read({"HDEL", sKey, sField}, iOutput);
}

[[nodiscard]] inline int32_t RedisHDelSync(const std::string& sKey, const std::string& sField, int64_t& iOutput)
{
	return Detail::ReadSync({"HDEL", sKey, sField}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHExists(const std::string& sKey, const std::string& sField, int64_t& iOutput)
{
	return Detail::Read({"HEXISTS", sKey, sField}, iOutput);
}

[[nodiscard]] inline int32_t RedisHExistsSync(const std::string& sKey, const std::string& sField, int64_t& iOutput)
{
	return Detail::ReadSync({"HEXISTS", sKey, sField}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHSet(const std::string& sKey, const std::string& sField, const T& stValue)
{
	return Detail::Status({"HSET", sKey, sField, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetSync(const std::string& sKey, const std::string& sField, const T& stValue)
{
	return Detail::StatusSync({"HSET", sKey, sField, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHSet(const std::string& sKey, const std::string& sField, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"HSET", sKey, sField, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetSync(const std::string& sKey, const std::string& sField, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"HSET", sKey, sField, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHSetNx(const std::string& sKey, const std::string& sField, const T& stValue, bool& bOutput)
{
	return Detail::Read({"HSETNX", sKey, sField, stValue}, bOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetNxSync(const std::string& sKey, const std::string& sField, const T& stValue, bool& bOutput)
{
	return Detail::ReadSync({"HSETNX", sKey, sField, stValue}, bOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHKeys(const std::string& sKey, std::vector<std::string>& vOutput)
{
	return Detail::Read({"HKEYS", sKey}, vOutput);
}

[[nodiscard]] inline int32_t RedisHKeysSync(const std::string& sKey, std::vector<std::string>& vOutput)
{
	return Detail::ReadSync({"HKEYS", sKey}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHVals(const std::string& sKey, std::vector<T>& vOutput)
{
	return Detail::Read({"HVALS", sKey}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHValsSync(const std::string& sKey, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"HVALS", sKey}, vOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHStrLen(const std::string& sKey, const std::string& sField, int64_t& iOutput)
{
	return Detail::Read({"HSTRLEN", sKey, sField}, iOutput);
}

[[nodiscard]] inline int32_t RedisHStrLenSync(const std::string& sKey, const std::string& sField, int64_t& iOutput)
{
	return Detail::ReadSync({"HSTRLEN", sKey, sField}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHIncrBy(const std::string& sKey, const std::string& sField, int64_t iDelta, int64_t& iOutput)
{
	return Detail::Read({"HINCRBY", sKey, sField, iDelta}, iOutput);
}

[[nodiscard]] inline int32_t RedisHIncrBySync(const std::string& sKey, const std::string& sField, int64_t iDelta, int64_t& iOutput)
{
	return Detail::ReadSync({"HINCRBY", sKey, sField, iDelta}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHIncrByFloat(const std::string& sKey, const std::string& sField, double dDelta, double& dOutput)
{
	return Detail::Read({"HINCRBYFLOAT", sKey, sField, dDelta}, dOutput);
}

[[nodiscard]] inline int32_t RedisHIncrByFloatSync(const std::string& sKey, const std::string& sField, double dDelta, double& dOutput)
{
	return Detail::ReadSync({"HINCRBYFLOAT", sKey, sField, dDelta}, dOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHMGet(const std::string& sKey, const std::vector<std::string>& vFields, std::vector<std::optional<T>>& vOutput)
{
	return Detail::Read(Detail::Members("HMGET", sKey, vFields), vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHMGetSync(const std::string& sKey, const std::vector<std::string>& vFields, std::vector<std::optional<T>>& vOutput)
{
	return Detail::ReadSync(Detail::Members("HMGET", sKey, vFields), vOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHDel(const std::string& sKey, const std::vector<std::string>& vFields, int64_t& iOutput)
{
	return Detail::Read(Detail::Members("HDEL", sKey, vFields), iOutput);
}

[[nodiscard]] inline int32_t RedisHDelSync(const std::string& sKey, const std::vector<std::string>& vFields, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::Members("HDEL", sKey, vFields), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHSet(const std::string& sKey, const std::map<std::string, T>& mValues)
{
	return Detail::Status(Detail::HashFields(sKey, mValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetSync(const std::string& sKey, const std::map<std::string, T>& mValues)
{
	return Detail::StatusSync(Detail::HashFields(sKey, mValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHSet(const std::string& sKey, const std::map<std::string, T>& mValues, int64_t& iOutput)
{
	return Detail::Read(Detail::HashFields(sKey, mValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetSync(const std::string& sKey, const std::map<std::string, T>& mValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::HashFields(sKey, mValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHScan(const std::string& sKey, uint64_t iCursor, RedisScanPage<std::map<std::string, T>>& stOutput, int64_t iCount = 100, const std::string& sPattern = "*")
{
	return Detail::Read({"HSCAN", sKey, std::to_string(iCursor), "MATCH", sPattern, "COUNT", iCount}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHScanSync(const std::string& sKey, uint64_t iCursor, RedisScanPage<std::map<std::string, T>>& stOutput, int64_t iCount = 100, const std::string& sPattern = "*")
{
	return Detail::ReadSync({"HSCAN", sKey, std::to_string(iCursor), "MATCH", sPattern, "COUNT", iCount}, stOutput);
}
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisHDel(const std::string& sKey, const std::vector<std::string>& vFields)
{
	return Detail::Status(Detail::Members("HDEL", sKey, vFields), REDIS_REPLY_INTEGER);
}

[[nodiscard]] inline int32_t RedisHDelSync(const std::string& sKey, const std::vector<std::string>& vFields)
{
	return Detail::StatusSync(Detail::Members("HDEL", sKey, vFields), REDIS_REPLY_INTEGER);
}

}
}
