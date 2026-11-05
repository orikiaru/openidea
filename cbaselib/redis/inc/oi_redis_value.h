#pragma once
#include "oi_redis.h"
#include "oi_rpc_coro.h"
#include "oi_rpc.h"
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <type_traits>

namespace OI
{
namespace Redis
{
template <typename T>
struct RedisScored
{
	T stMember{};
	double dScore = 0;
};

// 单 key 的一页扫描结果；游标为 0 表示本轮结束，COUNT 仅是服务端工作量提示。
template <typename T>
struct RedisScanPage
{
	uint64_t iCursor = 0;
	T stValues{};
};

namespace Detail
{
inline std::string Encode(const std::string& sValueParam)
{
	return sValueParam;
}
inline std::string Encode(const char* pValueParam)
{
	if (!pValueParam)
	{
		throw std::invalid_argument("null Redis value");
	}
	return pValueParam;
}
template <typename T>
    requires std::is_base_of_v<OI::Rpc::RpcData, T>
std::string Encode(const T& stValueParam)
{
	OI::Base::BytesStream stBytes;
	stValueParam.serialize(stBytes);
	return stBytes.Size() ? std::string(stBytes.Begin(), stBytes.Size()) : std::string();
}
inline std::string Score(double dValueParam)
{
	if (std::isnan(dValueParam))
	{
		throw std::invalid_argument("NaN Redis score");
	}
	char vBuffer[64];
	const auto stResult = std::to_chars(vBuffer, vBuffer + sizeof(vBuffer), dValueParam, std::chars_format::general, std::numeric_limits<double>::max_digits10);
	if (stResult.ec != std::errc())
	{
		throw std::invalid_argument("invalid Redis score");
	}
	return std::string(vBuffer, stResult.ptr);
}

inline std::string Encode(int64_t iValueParam)
{
	return std::to_string(iValueParam);
}
inline std::string Encode(double dValueParam)
{
	return Score(dValueParam);
}
struct Arguments
{
	std::vector<std::string> vValues;
	int32_t iRet = OI_OK;
	explicit Arguments(std::vector<std::string> vArgumentsParam)
	    : vValues(std::move(vArgumentsParam))
	{
	}
	template <typename... T>
	Arguments(const T&... stValuesParam)
	{
		try
		{
			vValues.reserve(sizeof...(T));
			(vValues.emplace_back(Encode(stValuesParam)), ...);
		}
		catch (const std::invalid_argument&)
		{
			iRet = OI_ERR_INVALID_ARGUMENT;
		}
		catch (...)
		{
			iRet = OI_ERR_ENCODE;
		}
	}
	template <typename T>
	void Append(const T& stValueParam)
	{
		if (iRet != OI_OK)
		{
			return;
		}
		try
		{
			vValues.push_back(Encode(stValueParam));
		}
		catch (const std::invalid_argument&)
		{
			iRet = OI_ERR_INVALID_ARGUMENT;
		}
		catch (...)
		{
			iRet = OI_ERR_ENCODE;
		}
	}
};

template <typename T>
Arguments Members(const char* pCommandParam, const std::string& sKeyParam, const std::vector<T>& vValuesParam)
{
	Arguments stArguments{pCommandParam, sKeyParam};
	if (vValuesParam.empty())
	{
		stArguments.iRet = OI_ERR_INVALID_ARGUMENT;
	}
	for (const auto& stValue : vValuesParam)
	{
		stArguments.Append(stValue);
	}
	return stArguments;
}
template <typename T>
Arguments HashFields(const std::string& sKeyParam, const std::map<std::string, T>& mValuesParam)
{
	Arguments stArguments{"HSET", sKeyParam};
	if (mValuesParam.empty())
	{
		stArguments.iRet = OI_ERR_INVALID_ARGUMENT;
	}
	for (const auto& [sField, stValue] : mValuesParam)
	{
		stArguments.Append(sField);
		stArguments.Append(stValue);
	}
	return stArguments;
}
template <typename T>
Arguments ScoredMembers(const std::string& sKeyParam, const std::vector<RedisScored<T>>& vValuesParam)
{
	Arguments stArguments{"ZADD", sKeyParam};
	if (vValuesParam.empty())
	{
		stArguments.iRet = OI_ERR_INVALID_ARGUMENT;
	}
	for (const auto& stValue : vValuesParam)
	{
		stArguments.Append(stValue.dScore);
		stArguments.Append(stValue.stMember);
	}
	return stArguments;
}

// Decode 只写临时对象，整份回复成功后才提交到应用输出。
template <typename T>
int32_t Decode(const redisReply& stReplyParam, T& stOutputParam)
{
	if (stReplyParam.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if constexpr (std::is_same_v<T, std::string>)
	{
		if (stReplyParam.type != REDIS_REPLY_STRING && stReplyParam.type != REDIS_REPLY_STATUS)
		{
			return OI_ERR_PROTOCOL;
		}
		if (!stReplyParam.str && stReplyParam.len)
		{
			return OI_ERR_PROTOCOL;
		}
		stOutputParam.assign(stReplyParam.str ? stReplyParam.str : "", stReplyParam.len);
	}
	else if constexpr (std::is_integral_v<T>)
	{
		if (stReplyParam.type != REDIS_REPLY_INTEGER || !std::in_range<T>(stReplyParam.integer))
		{
			return OI_ERR_PROTOCOL;
		}
		stOutputParam = static_cast<T>(stReplyParam.integer);
	}
	else if constexpr (std::is_same_v<T, double>)
	{
		if (stReplyParam.type != REDIS_REPLY_STRING || !stReplyParam.str || !stReplyParam.len)
		{
			return OI_ERR_PROTOCOL;
		}
		const auto stResult = std::from_chars(stReplyParam.str, stReplyParam.str + stReplyParam.len, stOutputParam, std::chars_format::general);
		if (stResult.ec != std::errc() || stResult.ptr != stReplyParam.str + stReplyParam.len || std::isnan(stOutputParam))
		{
			return OI_ERR_PROTOCOL;
		}
	}
	else
	{
		static_assert(std::is_base_of_v<OI::Rpc::RpcData, T>, "Redis value must be string or rpcdata");
		if (stReplyParam.type != REDIS_REPLY_STRING || (!stReplyParam.str && stReplyParam.len))
		{
			return OI_ERR_PROTOCOL;
		}
		try
		{
			OI::Base::BytesStream stBytes(stReplyParam.str ? stReplyParam.str : "", stReplyParam.len);
			stOutputParam.deserialize(stBytes);
			if (stBytes.RemainSize() != 0)
			{
				return OI_ERR_DECODE;
			}
		}
		catch (const OI::Base::ParserException&)
		{
			return OI_ERR_DECODE;
		}
	}
	return OI_OK;
}
// bool 不能用 std::in_range；整数回执只接受 Redis 的 0/1。
inline int32_t Decode(const redisReply& stReplyParam, bool& bOutputParam)
{
	if (stReplyParam.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stReplyParam.type != REDIS_REPLY_INTEGER || (stReplyParam.integer != 0 && stReplyParam.integer != 1))
	{
		return OI_ERR_PROTOCOL;
	}
	bOutputParam = stReplyParam.integer != 0;
	return OI_OK;
}
template <typename T>
int32_t Decode(const redisReply& stReplyParam, std::optional<T>& stOutputParam)
{
	if (stReplyParam.type == REDIS_REPLY_NIL)
	{
		stOutputParam.reset();
		return OI_OK;
	}
	T stValue{};
	const int32_t iRet = Decode(stReplyParam, stValue);
	if (iRet == OI_OK)
	{
		stOutputParam = std::move(stValue);
	}
	return iRet;
}

template <typename T>
int32_t Decode(const redisReply& stReplyParam, std::vector<T>& vOutputParam)
{
	static_assert(!std::is_same_v<T, bool>, "Use vector<int64_t> for Redis 0/1 arrays; vector<bool> has proxy references");
	if (stReplyParam.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stReplyParam.type != REDIS_REPLY_ARRAY || (stReplyParam.elements && !stReplyParam.element))
	{
		return OI_ERR_PROTOCOL;
	}
	vOutputParam.reserve(stReplyParam.elements);
	for (size_t iIndex = 0; iIndex < stReplyParam.elements; ++iIndex)
	{
		if (!stReplyParam.element[iIndex])
		{
			return OI_ERR_PROTOCOL;
		}
		T stValue{};
		const int32_t iRet = Decode(*stReplyParam.element[iIndex], stValue);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		vOutputParam.push_back(std::move(stValue));
	}
	return OI_OK;
}
template <typename T>
int32_t Decode(const redisReply& stReplyParam, std::vector<RedisScored<T>>& vOutputParam)
{
	if (stReplyParam.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stReplyParam.type != REDIS_REPLY_ARRAY || stReplyParam.elements % 2 || (stReplyParam.elements && !stReplyParam.element))
	{
		return OI_ERR_PROTOCOL;
	}
	vOutputParam.reserve(stReplyParam.elements / 2);
	for (size_t iIndex = 0; iIndex < stReplyParam.elements; iIndex += 2)
	{
		if (!stReplyParam.element[iIndex] || !stReplyParam.element[iIndex + 1])
		{
			return OI_ERR_PROTOCOL;
		}
		RedisScored<T> stValue;
		int32_t iRet = Decode(*stReplyParam.element[iIndex], stValue.stMember);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		iRet = Decode(*stReplyParam.element[iIndex + 1], stValue.dScore);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		vOutputParam.push_back(std::move(stValue));
	}
	return OI_OK;
}
template <typename T>
int32_t Decode(const redisReply& stReplyParam, std::map<std::string, T>& mOutputParam)
{
	if (stReplyParam.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stReplyParam.type != REDIS_REPLY_ARRAY || stReplyParam.elements % 2 || (stReplyParam.elements && !stReplyParam.element))
	{
		return OI_ERR_PROTOCOL;
	}
	for (size_t iIndex = 0; iIndex < stReplyParam.elements; iIndex += 2)
	{
		if (!stReplyParam.element[iIndex] || !stReplyParam.element[iIndex + 1])
		{
			return OI_ERR_PROTOCOL;
		}
		std::string sField;
		T stValue{};
		int32_t iRet = Decode(*stReplyParam.element[iIndex], sField);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		iRet = Decode(*stReplyParam.element[iIndex + 1], stValue);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		mOutputParam.insert_or_assign(std::move(sField), std::move(stValue));
	}
	return OI_OK;
}
template <typename T>
int32_t Decode(const redisReply& stReplyParam, RedisScanPage<T>& stOutputParam)
{
	if (stReplyParam.type != REDIS_REPLY_ARRAY || stReplyParam.elements != 2 || !stReplyParam.element || !stReplyParam.element[0] || !stReplyParam.element[1])
	{
		return OI_ERR_PROTOCOL;
	}
	const auto& stCursor = *stReplyParam.element[0];
	if (stCursor.type != REDIS_REPLY_STRING || !stCursor.str || !stCursor.len)
	{
		return OI_ERR_PROTOCOL;
	}
	const auto stResult = std::from_chars(stCursor.str, stCursor.str + stCursor.len, stOutputParam.iCursor);
	if (stResult.ec != std::errc() || stResult.ptr != stCursor.str + stCursor.len)
	{
		return OI_ERR_PROTOCOL;
	}
	return Decode(*stReplyParam.element[1], stOutputParam.stValues);
}

template <typename T>
int32_t ReadResult(const RedisResult& stResultParam, T& stOutputParam)
{
	const int32_t iRet = stResultParam.GetRet();
	if (iRet != OI_OK)
	{
		return iRet;
	}
	T stDecoded{};
	const int32_t iDecoded = Decode(*stResultParam.pReply, stDecoded);
	if (iDecoded == OI_OK)
	{
		stOutputParam = std::move(stDecoded);
	}
	return iDecoded;
}
inline int32_t StatusResult(const RedisResult& stResultParam, int iReplyTypeParam)
{
	const int32_t iRet = stResultParam.GetRet();
	if (iRet != OI_OK)
	{
		return iRet;
	}
	if (stResultParam.pReply->type != iReplyTypeParam)
	{
		return OI_ERR_PROTOCOL;
	}
	if (iReplyTypeParam == REDIS_REPLY_STATUS && (!stResultParam.pReply->str || std::string_view(stResultParam.pReply->str, stResultParam.pReply->len) != "OK"))
	{
		return OI_ERR_PROTOCOL;
	}
	return OI_OK;
}
template <typename T>
coro::TaskResult<int32_t> Read(Arguments stArgumentsParam, T& stOutputParam)
{
	if (stArgumentsParam.iRet != OI_OK)
	{
		co_return stArgumentsParam.iRet;
	}
	const auto stResult = co_await CallRedis(std::move(stArgumentsParam.vValues));
	co_return ReadResult(stResult, stOutputParam);
}
template <typename T>
int32_t ReadSync(Arguments stArgumentsParam, T& stOutputParam)
{
	if (stArgumentsParam.iRet != OI_OK)
	{
		return stArgumentsParam.iRet;
	}
	return ReadResult(CallRedisSync(std::move(stArgumentsParam.vValues)), stOutputParam);
}
inline coro::TaskResult<int32_t> Status(Arguments stArgumentsParam, int iReplyTypeParam)
{
	if (stArgumentsParam.iRet != OI_OK)
	{
		co_return stArgumentsParam.iRet;
	}
	co_return StatusResult(co_await CallRedis(std::move(stArgumentsParam.vValues)), iReplyTypeParam);
}
inline int32_t StatusSync(Arguments stArgumentsParam, int iReplyTypeParam)
{
	if (stArgumentsParam.iRet != OI_OK)
	{
		return stArgumentsParam.iRet;
	}
	return StatusResult(CallRedisSync(std::move(stArgumentsParam.vValues)), iReplyTypeParam);
}
}
}
}
