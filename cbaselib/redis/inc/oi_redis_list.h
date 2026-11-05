#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPop(const std::string& sKey, T& stOutput)
{
	return Detail::Read({"LPOP", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPopSync(const std::string& sKey, T& stOutput)
{
	return Detail::ReadSync({"LPOP", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPop(const std::string& sKey, T& stOutput)
{
	return Detail::Read({"RPOP", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPopSync(const std::string& sKey, T& stOutput)
{
	return Detail::ReadSync({"RPOP", sKey}, stOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLLen(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"LLEN", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisLLenSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"LLEN", sKey}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPush(const std::string& sKey, const T& stValue)
{
	return Detail::Status({"LPUSH", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushSync(const std::string& sKey, const T& stValue)
{
	return Detail::StatusSync({"LPUSH", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPush(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"LPUSH", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"LPUSH", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPush(const std::string& sKey, const T& stValue)
{
	return Detail::Status({"RPUSH", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushSync(const std::string& sKey, const T& stValue)
{
	return Detail::StatusSync({"RPUSH", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPush(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"RPUSH", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"RPUSH", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLRange(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<T>& vOutput)
{
	return Detail::Read({"LRANGE", sKey, iStart, iStop}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLRangeSync(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"LRANGE", sKey, iStart, iStop}, vOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLTrim(const std::string& sKey, int64_t iStart, int64_t iStop)
{
	return Detail::Status({"LTRIM", sKey, iStart, iStop}, REDIS_REPLY_STATUS);
}

[[nodiscard]] inline int32_t RedisLTrimSync(const std::string& sKey, int64_t iStart, int64_t iStop)
{
	return Detail::StatusSync({"LTRIM", sKey, iStart, iStop}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLSet(const std::string& sKey, int64_t iIndex, const T& stValue)
{
	return Detail::Status({"LSET", sKey, iIndex, stValue}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLSetSync(const std::string& sKey, int64_t iIndex, const T& stValue)
{
	return Detail::StatusSync({"LSET", sKey, iIndex, stValue}, REDIS_REPLY_STATUS);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLIndex(const std::string& sKey, int64_t iIndex, T& stOutput)
{
	return Detail::Read({"LINDEX", sKey, iIndex}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLIndexSync(const std::string& sKey, int64_t iIndex, T& stOutput)
{
	return Detail::ReadSync({"LINDEX", sKey, iIndex}, stOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPos(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"LPOS", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPosSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"LPOS", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLRem(const std::string& sKey, int64_t iCount, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"LREM", sKey, iCount, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLRemSync(const std::string& sKey, int64_t iCount, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"LREM", sKey, iCount, stValue}, iOutput);
}

template <typename T, typename U>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLInsertBefore(const std::string& sKey, const T& stPivot, const U& stValue, int64_t& iOutput)
{
	return Detail::Read({"LINSERT", sKey, "BEFORE", stPivot, stValue}, iOutput);
}

template <typename T, typename U>
[[nodiscard]] inline int32_t RedisLInsertBeforeSync(const std::string& sKey, const T& stPivot, const U& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"LINSERT", sKey, "BEFORE", stPivot, stValue}, iOutput);
}

template <typename T, typename U>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLInsertAfter(const std::string& sKey, const T& stPivot, const U& stValue, int64_t& iOutput)
{
	return Detail::Read({"LINSERT", sKey, "AFTER", stPivot, stValue}, iOutput);
}

template <typename T, typename U>
[[nodiscard]] inline int32_t RedisLInsertAfterSync(const std::string& sKey, const T& stPivot, const U& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"LINSERT", sKey, "AFTER", stPivot, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPushX(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"LPUSHX", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushXSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"LPUSHX", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPushX(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"RPUSHX", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushXSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"RPUSHX", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPush(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::Read(Detail::Members("LPUSH", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushSync(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::Members("LPUSH", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPush(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::Read(Detail::Members("RPUSH", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushSync(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::Members("RPUSH", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPushX(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::Read(Detail::Members("LPUSHX", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushXSync(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::Members("LPUSHX", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPushX(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::Read(Detail::Members("RPUSHX", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushXSync(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::Members("RPUSHX", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPop(const std::string& sKey, int64_t iCount, std::vector<T>& vOutput)
{
	return Detail::Read({"LPOP", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPopSync(const std::string& sKey, int64_t iCount, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"LPOP", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPop(const std::string& sKey, int64_t iCount, std::vector<T>& vOutput)
{
	return Detail::Read({"RPOP", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPopSync(const std::string& sKey, int64_t iCount, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"RPOP", sKey, iCount}, vOutput);
}
template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPush(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::Status(Detail::Members("LPUSH", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushSync(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::StatusSync(Detail::Members("LPUSH", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPush(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::Status(Detail::Members("RPUSH", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushSync(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::StatusSync(Detail::Members("RPUSH", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisLPushX(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::Status(Detail::Members("LPUSHX", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisLPushXSync(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::StatusSync(Detail::Members("LPUSHX", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisRPushX(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::Status(Detail::Members("RPUSHX", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisRPushXSync(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::StatusSync(Detail::Members("RPUSHX", sKey, vValues), REDIS_REPLY_INTEGER);
}

}
}
