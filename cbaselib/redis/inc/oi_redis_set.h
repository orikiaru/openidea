#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSCard(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"SCARD", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisSCardSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"SCARD", sKeyParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSAdd(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::Status({"SADD", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSAddSync(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::StatusSync({"SADD", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSAdd(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"SADD", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSAddSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"SADD", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSRem(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::Status({"SREM", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRemSync(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::StatusSync({"SREM", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSRem(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"SREM", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRemSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"SREM", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSIsMember(const std::string& sKeyParam, const T& stValueParam, bool& bOutputParam)
{
	return Detail::Read({"SISMEMBER", sKeyParam, stValueParam}, bOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSIsMemberSync(const std::string& sKeyParam, const T& stValueParam, bool& bOutputParam)
{
	return Detail::ReadSync({"SISMEMBER", sKeyParam, stValueParam}, bOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSMembers(const std::string& sKeyParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"SMEMBERS", sKeyParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSMembersSync(const std::string& sKeyParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"SMEMBERS", sKeyParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSAdd(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::Members("SADD", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSAddSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::Members("SADD", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSRem(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::Members("SREM", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRemSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::Members("SREM", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSPop(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::Read({"SPOP", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSPopSync(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::ReadSync({"SPOP", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSPop(const std::string& sKeyParam, int64_t iCountParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"SPOP", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSPopSync(const std::string& sKeyParam, int64_t iCountParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"SPOP", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSRandMember(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::Read({"SRANDMEMBER", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRandMemberSync(const std::string& sKeyParam, T& stOutputParam)
{
	return Detail::ReadSync({"SRANDMEMBER", sKeyParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSRandMember(const std::string& sKeyParam, int64_t iCountParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"SRANDMEMBER", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRandMemberSync(const std::string& sKeyParam, int64_t iCountParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"SRANDMEMBER", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSMIsMember(const std::string& sKeyParam, const std::vector<T>& vValuesParam, std::vector<int64_t>& vOutputParam)
{
	return Detail::Read(Detail::Members("SMISMEMBER", sKeyParam, vValuesParam), vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSMIsMemberSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, std::vector<int64_t>& vOutputParam)
{
	return Detail::ReadSync(Detail::Members("SMISMEMBER", sKeyParam, vValuesParam), vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSScan(const std::string& sKeyParam, uint64_t iCursorParam, RedisScanPage<std::vector<T>>& stOutputParam, int64_t iCountParam = 100, const std::string& sPatternParam = "*")
{
	return Detail::Read({"SSCAN", sKeyParam, std::to_string(iCursorParam), "MATCH", sPatternParam, "COUNT", iCountParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSScanSync(const std::string& sKeyParam, uint64_t iCursorParam, RedisScanPage<std::vector<T>>& stOutputParam, int64_t iCountParam = 100, const std::string& sPatternParam = "*")
{
	return Detail::ReadSync({"SSCAN", sKeyParam, std::to_string(iCursorParam), "MATCH", sPatternParam, "COUNT", iCountParam}, stOutputParam);
}
template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSAdd(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::Status(Detail::Members("SADD", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSAddSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::StatusSync(Detail::Members("SADD", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisSRem(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::Status(Detail::Members("SREM", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRemSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::StatusSync(Detail::Members("SREM", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

}
}
