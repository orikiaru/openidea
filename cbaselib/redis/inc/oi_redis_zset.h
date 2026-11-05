#pragma once
#include "oi_redis_value.h"
#include "oi_duration.h"

namespace OI
{
namespace Redis
{
// 单 key 接口：输入立即编码，输出对象必须活到调用完成。
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZCard(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZCARD", sKeyParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisZCardSync(const std::string& sKeyParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZCARD", sKeyParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRem(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::Status({"ZREM", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRemSync(const std::string& sKeyParam, const T& stValueParam)
{
	return Detail::StatusSync({"ZREM", sKeyParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRem(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZREM", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRemSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZREM", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRange(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"ZRANGE", sKeyParam, iStartParam, iStopParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeSync(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"ZRANGE", sKeyParam, iStartParam, iStopParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRangeWithScores(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<RedisScored<T>>& vOutputParam)
{
	return Detail::Read({"ZRANGE", sKeyParam, iStartParam, iStopParam, "WITHSCORES"}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeWithScoresSync(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<RedisScored<T>>& vOutputParam)
{
	return Detail::ReadSync({"ZRANGE", sKeyParam, iStartParam, iStopParam, "WITHSCORES"}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRevRange(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<T>& vOutputParam)
{
	return Detail::Read({"ZREVRANGE", sKeyParam, iStartParam, iStopParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeSync(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<T>& vOutputParam)
{
	return Detail::ReadSync({"ZREVRANGE", sKeyParam, iStartParam, iStopParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRevRangeWithScores(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<RedisScored<T>>& vOutputParam)
{
	return Detail::Read({"ZREVRANGE", sKeyParam, iStartParam, iStopParam, "WITHSCORES"}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeWithScoresSync(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, std::vector<RedisScored<T>>& vOutputParam)
{
	return Detail::ReadSync({"ZREVRANGE", sKeyParam, iStartParam, iStopParam, "WITHSCORES"}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZScore(const std::string& sKeyParam, const T& stValueParam, double& dOutputParam)
{
	return Detail::Read({"ZSCORE", sKeyParam, stValueParam}, dOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZScoreSync(const std::string& sKeyParam, const T& stValueParam, double& dOutputParam)
{
	return Detail::ReadSync({"ZSCORE", sKeyParam, stValueParam}, dOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRank(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZRANK", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRankSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZRANK", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRevRank(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZREVRANK", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRankSync(const std::string& sKeyParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZREVRANK", sKeyParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZAdd(const std::string& sKeyParam, double dScoreParam, const T& stValueParam)
{
	return Detail::Status({"ZADD", sKeyParam, dScoreParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddSync(const std::string& sKeyParam, double dScoreParam, const T& stValueParam)
{
	return Detail::StatusSync({"ZADD", sKeyParam, dScoreParam, stValueParam}, REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZAdd(const std::string& sKeyParam, double dScoreParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZADD", sKeyParam, dScoreParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddSync(const std::string& sKeyParam, double dScoreParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZADD", sKeyParam, dScoreParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZIncrBy(const std::string& sKeyParam, double dDeltaParam, const T& stValueParam, double& dOutputParam)
{
	return Detail::Read({"ZINCRBY", sKeyParam, dDeltaParam, stValueParam}, dOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZIncrBySync(const std::string& sKeyParam, double dDeltaParam, const T& stValueParam, double& dOutputParam)
{
	return Detail::ReadSync({"ZINCRBY", sKeyParam, dDeltaParam, stValueParam}, dOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisZCount(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZCOUNT", sKeyParam, sMinParam, sMaxParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisZCountSync(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZCOUNT", sKeyParam, sMinParam, sMaxParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisZLexCount(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZLEXCOUNT", sKeyParam, sMinParam, sMaxParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisZLexCountSync(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZLEXCOUNT", sKeyParam, sMinParam, sMaxParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRemRangeByScore(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZREMRANGEBYSCORE", sKeyParam, sMinParam, sMaxParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisZRemRangeByScoreSync(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZREMRANGEBYSCORE", sKeyParam, sMinParam, sMaxParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRemRangeByLex(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZREMRANGEBYLEX", sKeyParam, sMinParam, sMaxParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisZRemRangeByLexSync(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZREMRANGEBYLEX", sKeyParam, sMinParam, sMaxParam}, iOutputParam);
}

[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRemRangeByRank(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZREMRANGEBYRANK", sKeyParam, iStartParam, iStopParam}, iOutputParam);
}

[[nodiscard]] inline int32_t RedisZRemRangeByRankSync(const std::string& sKeyParam, int64_t iStartParam, int64_t iStopParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZREMRANGEBYRANK", sKeyParam, iStartParam, iStopParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRangeByScore(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, std::vector<T>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::Read({"ZRANGEBYSCORE", sKeyParam, sMinParam, sMaxParam, "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeByScoreSync(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, std::vector<T>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::ReadSync({"ZRANGEBYSCORE", sKeyParam, sMinParam, sMaxParam, "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRangeByScoreWithScores(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, std::vector<RedisScored<T>>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::Read({"ZRANGEBYSCORE", sKeyParam, sMinParam, sMaxParam, "WITHSCORES", "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeByScoreWithScoresSync(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, std::vector<RedisScored<T>>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::ReadSync({"ZRANGEBYSCORE", sKeyParam, sMinParam, sMaxParam, "WITHSCORES", "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRevRangeByScore(const std::string& sKeyParam, const std::string& sMaxParam, const std::string& sMinParam, std::vector<T>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::Read({"ZREVRANGEBYSCORE", sKeyParam, sMaxParam, sMinParam, "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeByScoreSync(const std::string& sKeyParam, const std::string& sMaxParam, const std::string& sMinParam, std::vector<T>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::ReadSync({"ZREVRANGEBYSCORE", sKeyParam, sMaxParam, sMinParam, "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRevRangeByScoreWithScores(const std::string& sKeyParam, const std::string& sMaxParam, const std::string& sMinParam, std::vector<RedisScored<T>>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::Read({"ZREVRANGEBYSCORE", sKeyParam, sMaxParam, sMinParam, "WITHSCORES", "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeByScoreWithScoresSync(const std::string& sKeyParam, const std::string& sMaxParam, const std::string& sMinParam, std::vector<RedisScored<T>>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::ReadSync({"ZREVRANGEBYSCORE", sKeyParam, sMaxParam, sMinParam, "WITHSCORES", "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRangeByLex(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, std::vector<T>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::Read({"ZRANGEBYLEX", sKeyParam, sMinParam, sMaxParam, "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRangeByLexSync(const std::string& sKeyParam, const std::string& sMinParam, const std::string& sMaxParam, std::vector<T>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::ReadSync({"ZRANGEBYLEX", sKeyParam, sMinParam, sMaxParam, "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRevRangeByLex(const std::string& sKeyParam, const std::string& sMaxParam, const std::string& sMinParam, std::vector<T>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::Read({"ZREVRANGEBYLEX", sKeyParam, sMaxParam, sMinParam, "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRevRangeByLexSync(const std::string& sKeyParam, const std::string& sMaxParam, const std::string& sMinParam, std::vector<T>& vOutputParam, int64_t iOffsetParam = 0, int64_t iCountParam = -1)
{
	return Detail::ReadSync({"ZREVRANGEBYLEX", sKeyParam, sMaxParam, sMinParam, "LIMIT", iOffsetParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZPopMin(const std::string& sKeyParam, std::vector<RedisScored<T>>& vOutputParam, int64_t iCountParam = 1)
{
	return Detail::Read({"ZPOPMIN", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZPopMinSync(const std::string& sKeyParam, std::vector<RedisScored<T>>& vOutputParam, int64_t iCountParam = 1)
{
	return Detail::ReadSync({"ZPOPMIN", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZPopMax(const std::string& sKeyParam, std::vector<RedisScored<T>>& vOutputParam, int64_t iCountParam = 1)
{
	return Detail::Read({"ZPOPMAX", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZPopMaxSync(const std::string& sKeyParam, std::vector<RedisScored<T>>& vOutputParam, int64_t iCountParam = 1)
{
	return Detail::ReadSync({"ZPOPMAX", sKeyParam, iCountParam}, vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZMScore(const std::string& sKeyParam, const std::vector<T>& vValuesParam, std::vector<std::optional<double>>& vOutputParam)
{
	return Detail::Read(Detail::Members("ZMSCORE", sKeyParam, vValuesParam), vOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZMScoreSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, std::vector<std::optional<double>>& vOutputParam)
{
	return Detail::ReadSync(Detail::Members("ZMSCORE", sKeyParam, vValuesParam), vOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZAdd(const std::string& sKeyParam, const std::vector<RedisScored<T>>& vValuesParam)
{
	return Detail::Status(Detail::ScoredMembers(sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddSync(const std::string& sKeyParam, const std::vector<RedisScored<T>>& vValuesParam)
{
	return Detail::StatusSync(Detail::ScoredMembers(sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZAdd(const std::string& sKeyParam, const std::vector<RedisScored<T>>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::ScoredMembers(sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddSync(const std::string& sKeyParam, const std::vector<RedisScored<T>>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::ScoredMembers(sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZAddNx(const std::string& sKeyParam, double dScoreParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZADD", sKeyParam, "NX", dScoreParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddNxSync(const std::string& sKeyParam, double dScoreParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZADD", sKeyParam, "NX", dScoreParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZAddXx(const std::string& sKeyParam, double dScoreParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZADD", sKeyParam, "XX", dScoreParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddXxSync(const std::string& sKeyParam, double dScoreParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZADD", sKeyParam, "XX", dScoreParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZAddChanged(const std::string& sKeyParam, double dScoreParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::Read({"ZADD", sKeyParam, "CH", dScoreParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZAddChangedSync(const std::string& sKeyParam, double dScoreParam, const T& stValueParam, int64_t& iOutputParam)
{
	return Detail::ReadSync({"ZADD", sKeyParam, "CH", dScoreParam, stValueParam}, iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZScan(const std::string& sKeyParam, uint64_t iCursorParam, RedisScanPage<std::vector<RedisScored<T>>>& stOutputParam, int64_t iCountParam = 100, const std::string& sPatternParam = "*")
{
	return Detail::Read({"ZSCAN", sKeyParam, std::to_string(iCursorParam), "MATCH", sPatternParam, "COUNT", iCountParam}, stOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZScanSync(const std::string& sKeyParam, uint64_t iCursorParam, RedisScanPage<std::vector<RedisScored<T>>>& stOutputParam, int64_t iCountParam = 100, const std::string& sPatternParam = "*")
{
	return Detail::ReadSync({"ZSCAN", sKeyParam, std::to_string(iCursorParam), "MATCH", sPatternParam, "COUNT", iCountParam}, stOutputParam);
}
template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRem(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::Read(Detail::Members("ZREM", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRemSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam, int64_t& iOutputParam)
{
	return Detail::ReadSync(Detail::Members("ZREM", sKeyParam, vValuesParam), iOutputParam);
}

template <typename T>
[[nodiscard]] inline coro::TaskResult<int32_t> RedisZRem(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::Status(Detail::Members("ZREM", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

template <typename T>
[[nodiscard]] inline int32_t RedisZRemSync(const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	return Detail::StatusSync(Detail::Members("ZREM", sKeyParam, vValuesParam), REDIS_REPLY_INTEGER);
}

}
}
