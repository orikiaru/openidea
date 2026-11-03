#pragma once
#include "oi_bytes.h"
#include "json/json.h"
#include <charconv>
#include <limits>
#include <type_traits>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace OI { namespace Rpc {

inline void ToJsonValue(Json::Value& stOutputParam, bool bInputParam) { stOutputParam = bInputParam; }
inline void ToJsonValue(Json::Value& stOutputParam, int8_t iInputParam) { stOutputParam = static_cast<int32_t>(iInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, uint8_t iInputParam) { stOutputParam = static_cast<uint32_t>(iInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, int16_t iInputParam) { stOutputParam = static_cast<int32_t>(iInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, uint16_t iInputParam) { stOutputParam = static_cast<uint32_t>(iInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, int32_t iInputParam) { stOutputParam = static_cast<Json::Int>(iInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, uint32_t iInputParam) { stOutputParam = static_cast<Json::UInt>(iInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, int64_t iInputParam) { stOutputParam = static_cast<Json::Int64>(iInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, uint64_t iInputParam) { stOutputParam = static_cast<Json::UInt64>(iInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, float fInputParam) { stOutputParam = static_cast<double>(fInputParam); }
inline void ToJsonValue(Json::Value& stOutputParam, double dInputParam) { stOutputParam = dInputParam; }
inline void ToJsonValue(Json::Value& stOutputParam, const std::string& sInputParam) { stOutputParam = sInputParam; }
inline void ToJsonValue(Json::Value& stOutputParam, const ::OI::Base::Bytes& stInputParam)
{
	stOutputParam = std::string(stInputParam.Data(), stInputParam.Data() ? stInputParam.Size() : 0);
}

inline bool FromJsonValue(const Json::Value& stInputParam, bool& bOutputParam)
{
	if (!stInputParam.isBool()) return false;
	bOutputParam = stInputParam.asBool();
	return true;
}
// 只接受整数 token，先检查目标范围，避免截断和有符号/无符号溢出。
template<typename T>
std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, bool>
FromJsonValue(const Json::Value& stInputParam, T& stOutputParam)
{
	if (stInputParam.type() == Json::intValue)
	{
		const auto iValue = stInputParam.asInt64();
		if constexpr (std::is_signed_v<T>)
		{
			if (iValue < (std::numeric_limits<T>::min)() || iValue > (std::numeric_limits<T>::max)()) return false;
		}
		else if (iValue < 0 || static_cast<Json::UInt64>(iValue) > (std::numeric_limits<T>::max)()) return false;
		stOutputParam = static_cast<T>(iValue);
		return true;
	}
	if (stInputParam.type() == Json::uintValue)
	{
		const auto iValue = stInputParam.asUInt64();
		if (iValue > static_cast<Json::UInt64>((std::numeric_limits<T>::max)())) return false;
		stOutputParam = static_cast<T>(iValue);
		return true;
	}
	return false;
}

// 在字符串有效期内完整解析整数键，不依赖 errno 或区域设置。
template<typename T>
bool ParseJsonIntegerKey(const std::string& sKeyParam, T& stOutputParam)
{
	T stValue{};
	const auto stParsed = std::from_chars(sKeyParam.data(), sKeyParam.data() + sKeyParam.size(), stValue);
	if (stParsed.ec != std::errc{} || stParsed.ptr != sKeyParam.data() + sKeyParam.size()) return false;
	stOutputParam = stValue;
	return true;
}

inline bool FromJsonValue(const Json::Value& stInputParam, float& fOutputParam)
{
	if (!stInputParam.isNumeric()) return false;
	fOutputParam = static_cast<float>(stInputParam.asDouble());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, double& dOutputParam)
{
	if (!stInputParam.isNumeric()) return false;
	dOutputParam = stInputParam.asDouble();
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, std::string& sOutputParam)
{
	if (!stInputParam.isString()) return false;
	sOutputParam = stInputParam.asString();
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, ::OI::Base::Bytes& stOutputParam)
{
	if (!stInputParam.isString()) return false;
	const std::string sText = stInputParam.asString();
	stOutputParam.Reserve(0);
	stOutputParam.Append(sText.data(), sText.size());
	return true;
}

template<typename T>
bool FromJsonValue(const Json::Value& stInputParam, std::vector<T>& stOutputParam)
{
	if (!stInputParam.isArray()) return false;
	std::vector<T> vDecoded;
	vDecoded.reserve(stInputParam.size());
	for (Json::ArrayIndex iIndexLocal = 0; iIndexLocal < stInputParam.size(); ++iIndexLocal)
	{
		T stElem{};
		if (!FromJsonValue(stInputParam[iIndexLocal], stElem)) return false;
		vDecoded.push_back(std::move(stElem));
	}
	stOutputParam.swap(vDecoded);
	return true;
}

template<typename T>
void ToJsonValue(Json::Value& stOutputParam, const std::vector<T>& stInputParam)
{
	stOutputParam = Json::Value(Json::arrayValue);
	for (size_t iIndexLocal = 0; iIndexLocal < stInputParam.size(); ++iIndexLocal)
	{
		Json::Value stChild;
		ToJsonValue(stChild, stInputParam[iIndexLocal]);
		stOutputParam.append(std::move(stChild));
	}
}

template<typename V>
bool FromJsonValue(const Json::Value& stInputParam, std::map<std::string, V>& stOutputParam)
{
	if (!stInputParam.isObject()) return false;
	std::map<std::string, V> mDecoded;
	for (auto it = stInputParam.begin(); it != stInputParam.end(); ++it)
	{
		V stVal{};
		if (!FromJsonValue(*it, stVal)) return false;
		mDecoded[it.name()] = std::move(stVal);
	}
	stOutputParam.swap(mDecoded);
	return true;
}

template<typename V>
void ToJsonValue(Json::Value& stOutputParam, const std::map<std::string, V>& stInputParam)
{
	stOutputParam = Json::Value(Json::objectValue);
	for (const auto& [sKey, stValue] : stInputParam)
	{
		Json::Value stChild;
		ToJsonValue(stChild, stValue);
		stOutputParam[sKey] = std::move(stChild);
	}
}

template<typename V>
bool FromJsonValue(const Json::Value& stInputParam, std::map<int32_t, V>& stOutputParam)
{
	if (!stInputParam.isObject()) return false;
	std::map<int32_t, V> mDecoded;
	for (auto it = stInputParam.begin(); it != stInputParam.end(); ++it)
	{
		int32_t iKey{};
		if (!ParseJsonIntegerKey(it.name(), iKey)) return false;
		V stVal{};
		if (!FromJsonValue(*it, stVal)) return false;
		mDecoded[iKey] = std::move(stVal);
	}
	stOutputParam.swap(mDecoded);
	return true;
}

template<typename V>
void ToJsonValue(Json::Value& stOutputParam, const std::map<int32_t, V>& stInputParam)
{
	stOutputParam = Json::Value(Json::objectValue);
	for (const auto& [iKey, stValue] : stInputParam)
	{
		Json::Value stChild;
		ToJsonValue(stChild, stValue);
		stOutputParam[std::to_string(iKey)] = std::move(stChild);
	}
}

template<typename U, typename V>
bool FromJsonUnsignedKeyMap(const Json::Value& stInputParam, std::map<U, V>& stOutputParam)
{
	if (!stInputParam.isObject()) return false;
	std::map<U, V> mDecoded;
	for (auto it = stInputParam.begin(); it != stInputParam.end(); ++it)
	{
		U stKey{};
		if (!ParseJsonIntegerKey(it.name(), stKey)) return false;
		V stVal{};
		if (!FromJsonValue(*it, stVal)) return false;
		mDecoded[stKey] = std::move(stVal);
	}
	stOutputParam.swap(mDecoded);
	return true;
}

template<typename U, typename V>
void ToJsonUnsignedKeyMap(Json::Value& stOutputParam, const std::map<U, V>& stInputParam)
{
	stOutputParam = Json::Value(Json::objectValue);
	for (const auto& [stKey, stValue] : stInputParam)
	{
		Json::Value stChild;
		ToJsonValue(stChild, stValue);
		stOutputParam[std::to_string(stKey)] = std::move(stChild);
	}
}

template<typename V>
bool FromJsonValue(const Json::Value& stInputParam, std::map<uint32_t, V>& stOutputParam)
{
	return FromJsonUnsignedKeyMap(stInputParam, stOutputParam);
}

template<typename V>
void ToJsonValue(Json::Value& stOutputParam, const std::map<uint32_t, V>& stInputParam)
{
	ToJsonUnsignedKeyMap(stOutputParam, stInputParam);
}

template<typename V>
bool FromJsonValue(const Json::Value& stInputParam, std::map<uint64_t, V>& stOutputParam)
{
	return FromJsonUnsignedKeyMap(stInputParam, stOutputParam);
}

template<typename V>
void ToJsonValue(Json::Value& stOutputParam, const std::map<uint64_t, V>& stInputParam)
{
	ToJsonUnsignedKeyMap(stOutputParam, stInputParam);
}

template<typename T>
auto FromJsonValue(const Json::Value& stInputParam, T& stOutputParam) -> decltype(stOutputParam.FromJson(stInputParam))
{
	if (!stInputParam.isObject()) return false;
	return stOutputParam.FromJson(stInputParam);
}

template<typename T>
auto ToJsonValue(Json::Value& stOutputParam, const T& stInputParam) -> decltype(stInputParam.ToJson(stOutputParam))
{
	stInputParam.ToJson(stOutputParam);
}

}}
