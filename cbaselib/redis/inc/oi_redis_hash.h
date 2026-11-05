#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHLen(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"HLEN", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisHLenSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"HLEN", sKeyParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHGet(const std::string& sKeyParam, const std::string& sFieldParam, T& stOutputParam)
{
	return Detail::Read({"HGET", sKeyParam, sFieldParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHGetSync(const std::string& sKeyParam, const std::string& sFieldParam, T& stOutputParam)
{
	return Detail::ReadSync({"HGET", sKeyParam, sFieldParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHGetAll(const std::string& sKeyParam, std::map<std::string, T>& mOutputParam)
{
	return Detail::Read({"HGETALL", sKeyParam}, mOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHGetAllSync(const std::string& sKeyParam, std::map<std::string, T>& mOutputParam)
{
	return Detail::ReadSync({"HGETALL", sKeyParam}, mOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisHDel(const std::string& sKeyParam, const std::string& sFieldParam, int64_t& iOutputParam)
{
	return Detail::Read({"HDEL", sKeyParam, sFieldParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisHDelSync(const std::string& sKeyParam, const std::string& sFieldParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"HDEL", sKeyParam, sFieldParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisHExists(const std::string& sKeyParam, const std::string& sFieldParam, int64_t& iOutputParam)
{
	return Detail::Read({"HEXISTS", sKeyParam, sFieldParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisHExistsSync(const std::string& sKeyParam, const std::string& sFieldParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"HEXISTS", sKeyParam, sFieldParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHSet(const std::string& sKeyParam, const std::string& sFieldParam, const T& stValueParam)
{
	return Detail::Status({"HSET", sKeyParam, sFieldParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetSync(const std::string& sKeyParam, const std::string& sFieldParam, const T& stValueParam)
{
	return Detail::StatusSync({"HSET", sKeyParam, sFieldParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHSet(const std::string& sKeyParam, const std::string& sFieldParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"HSET", sKeyParam, sFieldParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetSync(const std::string& sKeyParam, const std::string& sFieldParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"HSET", sKeyParam, sFieldParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHSetNx(const std::string& sKeyParam, const std::string& sFieldParam, const T& stValueParam, bool& bOutputParam)
{
	return Detail::Read({"HSETNX", sKeyParam, sFieldParam, stValueParam}, bOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetNxSync(const std::string& sKeyParam, const std::string& sFieldParam, const T& stValueParam, bool& bOutputParam)
{
	return Detail::ReadSync({"HSETNX", sKeyParam, sFieldParam, stValueParam}, bOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisHKeys(const std::string& sKeyParam, std::vector<std::string>& vOutputParam)
{
	return Detail::Read({"HKEYS", sKeyParam}, vOutputParam);
}

[[nodiscard]] inline int32_t RedisHKeysSync(const std::string& sKeyParam, std::vector<std::string>& vOutputParam)
{
	return Detail::ReadSync({"HKEYS", sKeyParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHVals(const std::string& sKeyParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"HVALS", sKeyParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHValsSync(const std::string& sKeyParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"HVALS", sKeyParam}, vOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisHStrLen(const std::string& sKeyParam, const std::string& sFieldParam, int64_t& iOutputParam)
{
	return Detail::Read({"HSTRLEN", sKeyParam, sFieldParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisHStrLenSync(const std::string& sKeyParam, const std::string& sFieldParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"HSTRLEN", sKeyParam, sFieldParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisHIncrBy(const std::string& sKeyParam, const std::string& sFieldParam, int64_t iDeltaParam, int64_t& iOutputParam)
{
	return Detail::Read({"HINCRBY", sKeyParam, sFieldParam, iDeltaParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisHIncrBySync(const std::string& sKeyParam, const std::string& sFieldParam, int64_t iDeltaParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"HINCRBY", sKeyParam, sFieldParam, iDeltaParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisHIncrByFloat(const std::string& sKeyParam, const std::string& sFieldParam, double dDeltaParam, double& dOutputParam)
{
	return Detail::Read({"HINCRBYFLOAT", sKeyParam, sFieldParam, dDeltaParam}, dOutputParam);
}

[[nodiscard]] inline int32_t RedisHIncrByFloatSync(const std::string& sKeyParam, const std::string& sFieldParam, double dDeltaParam, double& dOutputParam)
{
	return Detail::ReadSync({"HINCRBYFLOAT", sKeyParam, sFieldParam, dDeltaParam}, dOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHMGet(const std::string& sKeyParam, const std::vector<std::string>& vFieldsParam, std::vector<std::optional<T>>& vOutputParam)
{
	return Detail::Read(Detail::Members("HMGET", sKeyParam, vFieldsParam), vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHMGetSync(const std::string& sKeyParam, const std::vector<std::string>& vFieldsParam, std::vector<std::optional<T>>& vOutputParam)
{
	return Detail::ReadSync(Detail::Members("HMGET", sKeyParam, vFieldsParam), vOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisHDel(const std::string& sKeyParam, const std::vector<std::string>& vFieldsParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::Members("HDEL", sKeyParam, vFieldsParam), iOutputParam);
}

[[nodiscard]] inline int32_t RedisHDelSync(const std::string& sKeyParam, const std::vector<std::string>& vFieldsParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::Members("HDEL", sKeyParam, vFieldsParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHSet(const std::string& sKeyParam, const std::map<std::string, T>& mValuesParam)
{
	return Detail::Status(Detail::HashFields(sKeyParam, mValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetSync(const std::string& sKeyParam, const std::map<std::string, T>& mValuesParam)
{
	return Detail::StatusSync(Detail::HashFields(sKeyParam, mValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHSet(const std::string& sKeyParam, const std::map<std::string, T>& mValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::HashFields(sKeyParam, mValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHSetSync(const std::string& sKeyParam, const std::map<std::string, T>& mValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::HashFields(sKeyParam, mValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHScan(const std::string& sKeyParam, uint64_t iCursorParam, RedisScanPage<std::map<std::string, T>>& stOutputParam, int64_t iCountParam = 100, const std::string& sPatternParam = "*")
{
	return Detail::Read({"HSCAN", sKeyParam, std::to_string(iCursorParam), "MATCH", sPatternParam, "COUNT", iCountParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisHScanSync(const std::string& sKeyParam, uint64_t iCursorParam, RedisScanPage<std::map<std::string, T>>& stOutputParam, int64_t iCountParam = 100, const std::string& sPatternParam = "*")
{
	return Detail::ReadSync({"HSCAN", sKeyParam, std::to_string(iCursorParam), "MATCH", sPatternParam, "COUNT", iCountParam}, stOutputParam);
}
[[nodiscard]] inline coro::TaskResult<int32_t> RedisHDel(const std::string& sKeyParam, const std::vector<std::string>& vFieldsParam)
{
	return Detail::Status(Detail::Members("HDEL", sKeyParam, vFieldsParam), REDIS_REPLY_INTEGER);
}

[[nodiscard]] inline int32_t RedisHDelSync(const std::string& sKeyParam, const std::vector<std::string>& vFieldsParam)
{
	return Detail::StatusSync(Detail::Members("HDEL", sKeyParam, vFieldsParam), REDIS_REPLY_INTEGER);
}

}
}
