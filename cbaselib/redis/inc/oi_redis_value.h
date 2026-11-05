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
inline std::string Encode(const std::string& sValue)
{
	return sValue;
}
inline std::string Encode(const char* pValue)
{
	if (!pValue)
	{
		throw std::invalid_argument("null Redis value");
	}
	return pValue;
}
template <typename T>
    requires std::is_base_of_v<OI::Rpc::RpcData, T>
std::string Encode(const T& stValue)
{
	OI::Base::BytesStream stBytes;
	stValue.serialize(stBytes);
	return stBytes.Size() ? std::string(stBytes.Begin(), stBytes.Size()) : std::string();
}
inline std::string Score(double dValue)
{
	if (std::isnan(dValue))
	{
		throw std::invalid_argument("NaN Redis score");
	}
	char vBuffer[64];
	const auto stResult = std::to_chars(vBuffer, vBuffer + sizeof(vBuffer), dValue, std::chars_format::general, std::numeric_limits<double>::max_digits10);
	if (stResult.ec != std::errc())
	{
		throw std::invalid_argument("invalid Redis score");
	}
	return std::string(vBuffer, stResult.ptr);
}

inline std::string Encode(int64_t iValue)
{
	return std::to_string(iValue);
}
inline std::string Encode(double dValue)
{
	return Score(dValue);
}
struct Arguments
{
	std::vector<std::string> vValues;
	int32_t iRet = OI_OK;
	explicit Arguments(std::vector<std::string> vArguments)
	    : vValues(std::move(vArguments))
	{
	}
	template <typename... T>
	Arguments(const T&... stValues)
	{
		try
		{
			vValues.reserve(sizeof...(T));
			(vValues.emplace_back(Encode(stValues)), ...);
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
	void Append(const T& stValue)
	{
		if (iRet != OI_OK)
		{
			return;
		}
		try
		{
			vValues.push_back(Encode(stValue));
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
Arguments Members(const char* pCommand, const std::string& sKey, const std::vector<T>& vValues)
{
	Arguments stArguments{pCommand, sKey};
	if (vValues.empty())
	{
		stArguments.iRet = OI_ERR_INVALID_ARGUMENT;
	}
	for (const auto& stValue : vValues)
	{
		stArguments.Append(stValue);
	}
	return stArguments;
}
template <typename T>
Arguments HashFields(const std::string& sKey, const std::map<std::string, T>& mValues)
{
	Arguments stArguments{"HSET", sKey};
	if (mValues.empty())
	{
		stArguments.iRet = OI_ERR_INVALID_ARGUMENT;
	}
	for (const auto& [sField, stValue] : mValues)
	{
		stArguments.Append(sField);
		stArguments.Append(stValue);
	}
	return stArguments;
}
template <typename T>
Arguments ScoredMembers(const std::string& sKey, const std::vector<RedisScored<T>>& vValues)
{
	Arguments stArguments{"ZADD", sKey};
	if (vValues.empty())
	{
		stArguments.iRet = OI_ERR_INVALID_ARGUMENT;
	}
	for (const auto& stValue : vValues)
	{
		stArguments.Append(stValue.dScore);
		stArguments.Append(stValue.stMember);
	}
	return stArguments;
}

// Decode 只写临时对象，整份回复成功后才提交到应用输出。
template <typename T>
int32_t Decode(const redisReply& stReply, T& stOutput)
{
	if (stReply.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if constexpr (std::is_same_v<T, std::string>)
	{
		if (stReply.type != REDIS_REPLY_STRING && stReply.type != REDIS_REPLY_STATUS)
		{
			return OI_ERR_PROTOCOL;
		}
		if (!stReply.str && stReply.len)
		{
			return OI_ERR_PROTOCOL;
		}
		stOutput.assign(stReply.str ? stReply.str : "", stReply.len);
	}
	else if constexpr (std::is_integral_v<T>)
	{
		if (stReply.type != REDIS_REPLY_INTEGER || !std::in_range<T>(stReply.integer))
		{
			return OI_ERR_PROTOCOL;
		}
		stOutput = static_cast<T>(stReply.integer);
	}
	else if constexpr (std::is_same_v<T, double>)
	{
		if (stReply.type != REDIS_REPLY_STRING || !stReply.str || !stReply.len)
		{
			return OI_ERR_PROTOCOL;
		}
		const auto stResult = std::from_chars(stReply.str, stReply.str + stReply.len, stOutput, std::chars_format::general);
		if (stResult.ec != std::errc() || stResult.ptr != stReply.str + stReply.len || std::isnan(stOutput))
		{
			return OI_ERR_PROTOCOL;
		}
	}
	else
	{
		static_assert(std::is_base_of_v<OI::Rpc::RpcData, T>, "Redis value must be string or rpcdata");
		if (stReply.type != REDIS_REPLY_STRING || (!stReply.str && stReply.len))
		{
			return OI_ERR_PROTOCOL;
		}
		try
		{
			OI::Base::BytesStream stBytes(stReply.str ? stReply.str : "", stReply.len);
			stOutput.deserialize(stBytes);
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
inline int32_t Decode(const redisReply& stReply, bool& bOutput)
{
	if (stReply.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stReply.type != REDIS_REPLY_INTEGER || (stReply.integer != 0 && stReply.integer != 1))
	{
		return OI_ERR_PROTOCOL;
	}
	bOutput = stReply.integer != 0;
	return OI_OK;
}
template <typename T>
int32_t Decode(const redisReply& stReply, std::optional<T>& stOutput)
{
	if (stReply.type == REDIS_REPLY_NIL)
	{
		stOutput.reset();
		return OI_OK;
	}
	T stValue{};
	const int32_t iRet = Decode(stReply, stValue);
	if (iRet == OI_OK)
	{
		stOutput = std::move(stValue);
	}
	return iRet;
}

template <typename T>
int32_t Decode(const redisReply& stReply, std::vector<T>& vOutput)
{
	static_assert(!std::is_same_v<T, bool>, "Use vector<int64_t> for Redis 0/1 arrays; vector<bool> has proxy references");
	if (stReply.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stReply.type != REDIS_REPLY_ARRAY || (stReply.elements && !stReply.element))
	{
		return OI_ERR_PROTOCOL;
	}
	vOutput.reserve(stReply.elements);
	for (size_t iIndex = 0; iIndex < stReply.elements; ++iIndex)
	{
		if (!stReply.element[iIndex])
		{
			return OI_ERR_PROTOCOL;
		}
		T stValue{};
		const int32_t iRet = Decode(*stReply.element[iIndex], stValue);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		vOutput.push_back(std::move(stValue));
	}
	return OI_OK;
}
template <typename T>
int32_t Decode(const redisReply& stReply, std::vector<RedisScored<T>>& vOutput)
{
	if (stReply.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stReply.type != REDIS_REPLY_ARRAY || stReply.elements % 2 || (stReply.elements && !stReply.element))
	{
		return OI_ERR_PROTOCOL;
	}
	vOutput.reserve(stReply.elements / 2);
	for (size_t iIndex = 0; iIndex < stReply.elements; iIndex += 2)
	{
		if (!stReply.element[iIndex] || !stReply.element[iIndex + 1])
		{
			return OI_ERR_PROTOCOL;
		}
		RedisScored<T> stValue;
		int32_t iRet = Decode(*stReply.element[iIndex], stValue.stMember);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		iRet = Decode(*stReply.element[iIndex + 1], stValue.dScore);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		vOutput.push_back(std::move(stValue));
	}
	return OI_OK;
}
template <typename T>
int32_t Decode(const redisReply& stReply, std::map<std::string, T>& mOutput)
{
	if (stReply.type == REDIS_REPLY_NIL)
	{
		return OI_ERR_NOT_FOUND;
	}
	if (stReply.type != REDIS_REPLY_ARRAY || stReply.elements % 2 || (stReply.elements && !stReply.element))
	{
		return OI_ERR_PROTOCOL;
	}
	for (size_t iIndex = 0; iIndex < stReply.elements; iIndex += 2)
	{
		if (!stReply.element[iIndex] || !stReply.element[iIndex + 1])
		{
			return OI_ERR_PROTOCOL;
		}
		std::string sField;
		T stValue{};
		int32_t iRet = Decode(*stReply.element[iIndex], sField);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		iRet = Decode(*stReply.element[iIndex + 1], stValue);
		if (iRet != OI_OK)
		{
			return iRet;
		}
		mOutput.insert_or_assign(std::move(sField), std::move(stValue));
	}
	return OI_OK;
}
template <typename T>
int32_t Decode(const redisReply& stReply, RedisScanPage<T>& stOutput)
{
	if (stReply.type != REDIS_REPLY_ARRAY || stReply.elements != 2 || !stReply.element || !stReply.element[0] || !stReply.element[1])
	{
		return OI_ERR_PROTOCOL;
	}
	const auto& stCursor = *stReply.element[0];
	if (stCursor.type != REDIS_REPLY_STRING || !stCursor.str || !stCursor.len)
	{
		return OI_ERR_PROTOCOL;
	}
	const auto stResult = std::from_chars(stCursor.str, stCursor.str + stCursor.len, stOutput.iCursor);
	if (stResult.ec != std::errc() || stResult.ptr != stCursor.str + stCursor.len)
	{
		return OI_ERR_PROTOCOL;
	}
	return Decode(*stReply.element[1], stOutput.stValues);
}

template <typename T>
int32_t ReadResult(const RedisResult& stResult, T& stOutput)
{
	const int32_t iRet = stResult.GetRet();
	if (iRet != OI_OK)
	{
		return iRet;
	}
	T stDecoded{};
	const int32_t iDecoded = Decode(*stResult.pReply, stDecoded);
	if (iDecoded == OI_OK)
	{
		stOutput = std::move(stDecoded);
	}
	return iDecoded;
}
inline int32_t StatusResult(const RedisResult& stResult, int iReplyType)
{
	const int32_t iRet = stResult.GetRet();
	if (iRet != OI_OK)
	{
		return iRet;
	}
	if (stResult.pReply->type != iReplyType)
	{
		return OI_ERR_PROTOCOL;
	}
	if (iReplyType == REDIS_REPLY_STATUS && (!stResult.pReply->str || std::string_view(stResult.pReply->str, stResult.pReply->len) != "OK"))
	{
		return OI_ERR_PROTOCOL;
	}
	return OI_OK;
}
template <typename T>
OI::Coro::TaskResult<int32_t> Read(Arguments stArguments, T& stOutput)
{
	if (stArguments.iRet != OI_OK)
	{
		co_return stArguments.iRet;
	}
	const auto stResult = co_await CallRedis(std::move(stArguments.vValues));
	co_return ReadResult(stResult, stOutput);
}
template <typename T>
int32_t ReadSync(Arguments stArguments, T& stOutput)
{
	if (stArguments.iRet != OI_OK)
	{
		return stArguments.iRet;
	}
	return ReadResult(CallRedisSync(std::move(stArguments.vValues)), stOutput);
}
inline OI::Coro::TaskResult<int32_t> Status(Arguments stArguments, int iReplyType)
{
	if (stArguments.iRet != OI_OK)
	{
		co_return stArguments.iRet;
	}
	co_return StatusResult(co_await CallRedis(std::move(stArguments.vValues)), iReplyType);
}
inline int32_t StatusSync(Arguments stArguments, int iReplyType)
{
	if (stArguments.iRet != OI_OK)
	{
		return stArguments.iRet;
	}
	return StatusResult(CallRedisSync(std::move(stArguments.vValues)), iReplyType);
}
}
}
}
