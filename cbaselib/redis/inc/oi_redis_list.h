#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPop(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::Read({"LPOP", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPopSync(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::ReadSync({"LPOP", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPop(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::Read({"RPOP", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPopSync(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::ReadSync({"RPOP", sKeyParam}, stOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisLLen(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"LLEN", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisLLenSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"LLEN", sKeyParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPush(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::Status({"LPUSH", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushSync(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::StatusSync({"LPUSH", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPush(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"LPUSH", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"LPUSH", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPush(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::Status({"RPUSH", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushSync(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::StatusSync({"RPUSH", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPush(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"RPUSH", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"RPUSH", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLRange(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"LRANGE", sKeyParam, iStartParam, iStopParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLRangeSync(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"LRANGE", sKeyParam, iStartParam, iStopParam}, vOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisLTrim(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam)
{
	return Detail::Status({"LTRIM", sKeyParam, iStartParam, iStopParam}, REDIS_REPLY_STATUS);
}

[[nodiscard]] inline int32_t RedisLTrimSync(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam)
{
	return Detail::StatusSync({"LTRIM", sKeyParam, iStartParam, iStopParam}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLSet(const std::string& sKeyParam, int64_t iIndexParam, const T& stValueParam)
{
	return Detail::Status({"LSET", sKeyParam, iIndexParam, stValueParam}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLSetSync(const std::string& sKeyParam, int64_t iIndexParam, const T& stValueParam)
{
	return Detail::StatusSync({"LSET", sKeyParam, iIndexParam, stValueParam}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLIndex(const std::string& sKeyParam, int64_t iIndexParam, T& stOutputParam)
{
	return Detail::Read({"LINDEX", sKeyParam, iIndexParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLIndexSync(const std::string& sKeyParam, int64_t iIndexParam, T& stOutputParam)
{
	return Detail::ReadSync({"LINDEX", sKeyParam, iIndexParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPos(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"LPOS", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPosSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"LPOS", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLRem(const std::string& sKeyParam, int64_t iCountParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"LREM", sKeyParam, iCountParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLRemSync(const std::string& sKeyParam, int64_t iCountParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"LREM", sKeyParam, iCountParam, stValueParam}, iOutputParam);
}

template <typename T, typename U>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLInsertBefore(const std::string& sKeyParam, const T& stPivotParam, const U& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"LINSERT", sKeyParam, "BEFORE", stPivotParam, stValueParam}, iOutputParam);
}

template <typename T, typename U>
[[nodiscard]] inline int32_t RedisLInsertBeforeSync(const std::string& sKeyParam, const T& stPivotParam, const U& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"LINSERT", sKeyParam, "BEFORE", stPivotParam, stValueParam}, iOutputParam);
}

template <typename T, typename U>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLInsertAfter(const std::string& sKeyParam, const T& stPivotParam, const U& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"LINSERT", sKeyParam, "AFTER", stPivotParam, stValueParam}, iOutputParam);
}

template <typename T, typename U>
[[nodiscard]] inline int32_t RedisLInsertAfterSync(const std::string& sKeyParam, const T& stPivotParam, const U& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"LINSERT", sKeyParam, "AFTER", stPivotParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPushX(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"LPUSHX", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushXSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"LPUSHX", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPushX(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"RPUSHX", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushXSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"RPUSHX", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPush(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::Members("LPUSH", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::Members("LPUSH", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPush(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::Members("RPUSH", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::Members("RPUSH", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPushX(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::Members("LPUSHX", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushXSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::Members("LPUSHX", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPushX(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::Members("RPUSHX", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushXSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::Members("RPUSHX", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPop(const std::string& sKeyParam, int64_t iCountParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"LPOP", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPopSync(const std::string& sKeyParam, int64_t iCountParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"LPOP", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPop(const std::string& sKeyParam, int64_t iCountParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"RPOP", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPopSync(const std::string& sKeyParam, int64_t iCountParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"RPOP", sKeyParam, iCountParam}, vOutputParam);
}
template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPush(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::Status(Detail::Members("LPUSH", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::StatusSync(Detail::Members("LPUSH", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPush(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::Status(Detail::Members("RPUSH", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::StatusSync(Detail::Members("RPUSH", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisLPushX(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::Status(Detail::Members("LPUSHX", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushXSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::StatusSync(Detail::Members("LPUSHX", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisRPushX(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::Status(Detail::Members("RPUSHX", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushXSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::StatusSync(Detail::Members("RPUSHX", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

}
}
