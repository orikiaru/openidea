#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZCard(const std::string& sKey, int64_t& iOutput)
{
	return Detail::Read({"ZCARD", sKey}, iOutput);
}

[[nodiscard]] inline int32_t RedisZCardSync(const std::string& sKey, int64_t& iOutput)
{
	return Detail::ReadSync({"ZCARD", sKey}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRem(const std::string& sKey, const T& stValue)
{
	return Detail::Status({"ZREM", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRemSync(const std::string& sKey, const T& stValue)
{
	return Detail::StatusSync({"ZREM", sKey, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRem(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"ZREM", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRemSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"ZREM", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRange(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<T>& vOutput)
{
	return Detail::Read({"ZRANGE", sKey, iStart, iStop}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeSync(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"ZRANGE", sKey, iStart, iStop}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRangeWithScores(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<RedisScored<T>>& vOutput)
{
	return Detail::Read({"ZRANGE", sKey, iStart, iStop, "WITHSCORES"}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeWithScoresSync(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<RedisScored<T>>& vOutput)
{
	return Detail::ReadSync({"ZRANGE", sKey, iStart, iStop, "WITHSCORES"}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRevRange(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<T>& vOutput)
{
	return Detail::Read({"ZREVRANGE", sKey, iStart, iStop}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeSync(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<T>& vOutput)
{
	return Detail::ReadSync({"ZREVRANGE", sKey, iStart, iStop}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRevRangeWithScores(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<RedisScored<T>>& vOutput)
{
	return Detail::Read({"ZREVRANGE", sKey, iStart, iStop, "WITHSCORES"}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeWithScoresSync(const std::string& sKey, int64_t iStart, int64_t iStop, std::vector<RedisScored<T>>& vOutput)
{
	return Detail::ReadSync({"ZREVRANGE", sKey, iStart, iStop, "WITHSCORES"}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZScore(const std::string& sKey, const T& stValue, double& dOutput)
{
	return Detail::Read({"ZSCORE", sKey, stValue}, dOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZScoreSync(const std::string& sKey, const T& stValue, double& dOutput)
{
	return Detail::ReadSync({"ZSCORE", sKey, stValue}, dOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRank(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"ZRANK", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRankSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"ZRANK", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRevRank(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"ZREVRANK", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRankSync(const std::string& sKey, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"ZREVRANK", sKey, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZAdd(const std::string& sKey, double dScore, const T& stValue)
{
	return Detail::Status({"ZADD", sKey, dScore, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddSync(const std::string& sKey, double dScore, const T& stValue)
{
	return Detail::StatusSync({"ZADD", sKey, dScore, stValue}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZAdd(const std::string& sKey, double dScore, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"ZADD", sKey, dScore, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddSync(const std::string& sKey, double dScore, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"ZADD", sKey, dScore, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZIncrBy(const std::string& sKey, double dDelta, const T& stValue, double& dOutput)
{
	return Detail::Read({"ZINCRBY", sKey, dDelta, stValue}, dOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZIncrBySync(const std::string& sKey, double dDelta, const T& stValue, double& dOutput)
{
	return Detail::ReadSync({"ZINCRBY", sKey, dDelta, stValue}, dOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZCount(const std::string& sKey, const std::string& sMin, const std::string& sMax, int64_t& iOutput)
{
	return Detail::Read({"ZCOUNT", sKey, sMin, sMax}, iOutput);
}

[[nodiscard]] inline int32_t RedisZCountSync(const std::string& sKey, const std::string& sMin, const std::string& sMax, int64_t& iOutput)
{
	return Detail::ReadSync({"ZCOUNT", sKey, sMin, sMax}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZLexCount(const std::string& sKey, const std::string& sMin, const std::string& sMax, int64_t& iOutput)
{
	return Detail::Read({"ZLEXCOUNT", sKey, sMin, sMax}, iOutput);
}

[[nodiscard]] inline int32_t RedisZLexCountSync(const std::string& sKey, const std::string& sMin, const std::string& sMax, int64_t& iOutput)
{
	return Detail::ReadSync({"ZLEXCOUNT", sKey, sMin, sMax}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRemRangeByScore(const std::string& sKey, const std::string& sMin, const std::string& sMax, int64_t& iOutput)
{
	return Detail::Read({"ZREMRANGEBYSCORE", sKey, sMin, sMax}, iOutput);
}

[[nodiscard]] inline int32_t RedisZRemRangeByScoreSync(const std::string& sKey, const std::string& sMin, const std::string& sMax, int64_t& iOutput)
{
	return Detail::ReadSync({"ZREMRANGEBYSCORE", sKey, sMin, sMax}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRemRangeByLex(const std::string& sKey, const std::string& sMin, const std::string& sMax, int64_t& iOutput)
{
	return Detail::Read({"ZREMRANGEBYLEX", sKey, sMin, sMax}, iOutput);
}

[[nodiscard]] inline int32_t RedisZRemRangeByLexSync(const std::string& sKey, const std::string& sMin, const std::string& sMax, int64_t& iOutput)
{
	return Detail::ReadSync({"ZREMRANGEBYLEX", sKey, sMin, sMax}, iOutput);
}

[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRemRangeByRank(const std::string& sKey, int64_t iStart, int64_t iStop, int64_t& iOutput)
{
	return Detail::Read({"ZREMRANGEBYRANK", sKey, iStart, iStop}, iOutput);
}

[[nodiscard]] inline int32_t RedisZRemRangeByRankSync(const std::string& sKey, int64_t iStart, int64_t iStop, int64_t& iOutput)
{
	return Detail::ReadSync({"ZREMRANGEBYRANK", sKey, iStart, iStop}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRangeByScore(const std::string& sKey, const std::string& sMin, const std::string& sMax, std::vector<T>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::Read({"ZRANGEBYSCORE", sKey, sMin, sMax, "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeByScoreSync(const std::string& sKey, const std::string& sMin, const std::string& sMax, std::vector<T>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::ReadSync({"ZRANGEBYSCORE", sKey, sMin, sMax, "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRangeByScoreWithScores(const std::string& sKey, const std::string& sMin, const std::string& sMax, std::vector<RedisScored<T>>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::Read({"ZRANGEBYSCORE", sKey, sMin, sMax, "WITHSCORES", "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeByScoreWithScoresSync(const std::string& sKey, const std::string& sMin, const std::string& sMax, std::vector<RedisScored<T>>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::ReadSync({"ZRANGEBYSCORE", sKey, sMin, sMax, "WITHSCORES", "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRevRangeByScore(const std::string& sKey, const std::string& sMax, const std::string& sMin, std::vector<T>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::Read({"ZREVRANGEBYSCORE", sKey, sMax, sMin, "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeByScoreSync(const std::string& sKey, const std::string& sMax, const std::string& sMin, std::vector<T>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::ReadSync({"ZREVRANGEBYSCORE", sKey, sMax, sMin, "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRevRangeByScoreWithScores(const std::string& sKey, const std::string& sMax, const std::string& sMin, std::vector<RedisScored<T>>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::Read({"ZREVRANGEBYSCORE", sKey, sMax, sMin, "WITHSCORES", "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeByScoreWithScoresSync(const std::string& sKey, const std::string& sMax, const std::string& sMin, std::vector<RedisScored<T>>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::ReadSync({"ZREVRANGEBYSCORE", sKey, sMax, sMin, "WITHSCORES", "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRangeByLex(const std::string& sKey, const std::string& sMin, const std::string& sMax, std::vector<T>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::Read({"ZRANGEBYLEX", sKey, sMin, sMax, "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeByLexSync(const std::string& sKey, const std::string& sMin, const std::string& sMax, std::vector<T>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::ReadSync({"ZRANGEBYLEX", sKey, sMin, sMax, "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRevRangeByLex(const std::string& sKey, const std::string& sMax, const std::string& sMin, std::vector<T>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::Read({"ZREVRANGEBYLEX", sKey, sMax, sMin, "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeByLexSync(const std::string& sKey, const std::string& sMax, const std::string& sMin, std::vector<T>& vOutput, int64_t iOffset = 0, int64_t iCount = -1)
{
	return Detail::ReadSync({"ZREVRANGEBYLEX", sKey, sMax, sMin, "LIMIT", iOffset, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZPopMin(const std::string& sKey, std::vector<RedisScored<T>>& vOutput, int64_t iCount = 1)
{
	return Detail::Read({"ZPOPMIN", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZPopMinSync(const std::string& sKey, std::vector<RedisScored<T>>& vOutput, int64_t iCount = 1)
{
	return Detail::ReadSync({"ZPOPMIN", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZPopMax(const std::string& sKey, std::vector<RedisScored<T>>& vOutput, int64_t iCount = 1)
{
	return Detail::Read({"ZPOPMAX", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZPopMaxSync(const std::string& sKey, std::vector<RedisScored<T>>& vOutput, int64_t iCount = 1)
{
	return Detail::ReadSync({"ZPOPMAX", sKey, iCount}, vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZMScore(const std::string& sKey, const std::vector<T>& vValues, std::vector<std::optional<double>>& vOutput)
{
	return Detail::Read(Detail::Members("ZMSCORE", sKey, vValues), vOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZMScoreSync(const std::string& sKey, const std::vector<T>& vValues, std::vector<std::optional<double>>& vOutput)
{
	return Detail::ReadSync(Detail::Members("ZMSCORE", sKey, vValues), vOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZAdd(const std::string& sKey, const std::vector<RedisScored<T>>& vValues)
{
	return Detail::Status(Detail::ScoredMembers(sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddSync(const std::string& sKey, const std::vector<RedisScored<T>>& vValues)
{
	return Detail::StatusSync(Detail::ScoredMembers(sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZAdd(const std::string& sKey, const std::vector<RedisScored<T>>& vValues, int64_t& iOutput)
{
	return Detail::Read(Detail::ScoredMembers(sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddSync(const std::string& sKey, const std::vector<RedisScored<T>>& vValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::ScoredMembers(sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZAddNx(const std::string& sKey, double dScore, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"ZADD", sKey, "NX", dScore, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddNxSync(const std::string& sKey, double dScore, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"ZADD", sKey, "NX", dScore, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZAddXx(const std::string& sKey, double dScore, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"ZADD", sKey, "XX", dScore, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddXxSync(const std::string& sKey, double dScore, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"ZADD", sKey, "XX", dScore, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZAddChanged(const std::string& sKey, double dScore, const T& stValue, int64_t& iOutput)
{
	return Detail::Read({"ZADD", sKey, "CH", dScore, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddChangedSync(const std::string& sKey, double dScore, const T& stValue, int64_t& iOutput)
{
	return Detail::ReadSync({"ZADD", sKey, "CH", dScore, stValue}, iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZScan(const std::string& sKey, uint64_t iCursor, RedisScanPage<std::vector<RedisScored<T>>>& stOutput, int64_t iCount = 100, const std::string& sPattern = "*")
{
	return Detail::Read({"ZSCAN", sKey, std::to_string(iCursor), "MATCH", sPattern, "COUNT", iCount}, stOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZScanSync(const std::string& sKey, uint64_t iCursor, RedisScanPage<std::vector<RedisScored<T>>>& stOutput, int64_t iCount = 100, const std::string& sPattern = "*")
{
	return Detail::ReadSync({"ZSCAN", sKey, std::to_string(iCursor), "MATCH", sPattern, "COUNT", iCount}, stOutput);
}
template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRem(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::Read(Detail::Members("ZREM", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRemSync(const std::string& sKey, const std::vector<T>& vValues, int64_t& iOutput)
{
	return Detail::ReadSync(Detail::Members("ZREM", sKey, vValues), iOutput);
}

template <typename T>
[[nodiscard]] inline OI::Coro::TaskResult<int32_t> RedisZRem(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::Status(Detail::Members("ZREM", sKey, vValues), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRemSync(const std::string& sKey, const std::vector<T>& vValues)
{
	return Detail::StatusSync(Detail::Members("ZREM", sKey, vValues), REDIS_REPLY_INTEGER);
}

}
}
