#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSCard(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"SCARD", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisSCardSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"SCARD", sKey}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSAdd(const std::string& sKey, const T& stValue)
{
	return Detail::Status({"SADD", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSAddSync(const std::string& sKey, const T& stValue)
{
	return Detail::StatusSync({"SADD", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSAdd(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"SADD", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSAddSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"SADD", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSRem(const std::string& sKey, const T& stValue)
{
	return Detail::Status({"SREM", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRemSync(const std::string& sKey, const T& stValue)
{
	return Detail::StatusSync({"SREM", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSRem(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"SREM", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRemSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"SREM", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSIsMember(const std::string& sKey, const T& stValue, bool& bOutput)
{
	return Detail::Read({"SISMEMBER", sKey, stValue}, bOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSIsMemberSync(const std::string& sKey, const T& stValue, bool& bOutput)
{
	return Detail::ReadSync({"SISMEMBER", sKey, stValue}, bOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSMembers(const std::string& sKey, std::vector<T>& vOutput)
{
	return Detail::Read({"SMEMBERS", sKey}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSMembersSync(const std::string& sKey, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"SMEMBERS", sKey}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSAdd(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::Read(Detail::Members("SADD", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSAddSync(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::Members("SADD", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSRem(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::Read(Detail::Members("SREM", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRemSync(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::Members("SREM", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSPop(const std::string& sKey, T& stOutput)
{
	return Detail::Read({"SPOP", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSPopSync(const std::string& sKey, T& stOutput)
{
	return Detail::ReadSync({"SPOP", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSPop(const std::string& sKey, int64_t iCount, std::vector<T>& vOutput)
{
	return Detail::Read({"SPOP", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSPopSync(const std::string& sKey, int64_t iCount, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"SPOP", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSRandMember(const std::string& sKey, T& stOutput)
{
	return Detail::Read({"SRANDMEMBER", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRandMemberSync(const std::string& sKey, T& stOutput)
{
	return Detail::ReadSync({"SRANDMEMBER", sKey}, stOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSRandMember(const std::string& sKey, int64_t iCount, std::vector<T>& vOutput)
{
	return Detail::Read({"SRANDMEMBER", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRandMemberSync(const std::string& sKey, int64_t iCount, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"SRANDMEMBER", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSMIsMember(const std::string& sKey, const std::vector<T>& vValues, std::vector<int64_t>& vOutput)
{
	return Detail::Read(Detail::Members("SMISMEMBER", sKey, vValues), vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSMIsMemberSync(const std::string& sKey, const std::vector<T>& vValues, std::vector<int64_t>& vOutput)
{
	return Detail::ReadSync(Detail::Members("SMISMEMBER", sKey, vValues), vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSScan(const std::string& sKey, uint64_t iCursor, RedisScanPage<std::vector<T>>& stOutput, int64_t iCount = 100, const std::string& sPattern = "*")
{
	return Detail::Read({"SSCAN", sKey, std::to_string(iCursor), "MATCH", sPattern, "COUNT", iCount}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSScanSync(const std::string& sKey, uint64_t iCursor, RedisScanPage<std::vector<T>>& stOutput, int64_t iCount = 100, const std::string& sPattern = "*")
{
	return Detail::ReadSync({"SSCAN", sKey, std::to_string(iCursor), "MATCH", sPattern, "COUNT", iCount}, stOutput);
}
template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSAdd(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::Status(Detail::Members("SADD", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSAddSync(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::StatusSync(Detail::Members("SADD", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisSRem(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::Status(Detail::Members("SREM", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisSRemSync(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::StatusSync(Detail::Members("SREM", sKey, vValues), REDIS_REPLY_INTEGER);
}

}
}
