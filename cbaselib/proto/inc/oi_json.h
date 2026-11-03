#pragma once
#include "oi_bytes.h"
#include "json/json.h"
#include <cerrno>
#include <cstdint>
#include <cstdlib>
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
inline bool FromJsonValue(const Json::Value& stInputParam, int8_t& iOutputParam)
{
	if (!stInputParam.isIntegral()) return false;
	iOutputParam = static_cast<int8_t>(stInputParam.asInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, uint8_t& iOutputParam)
{
	if (!stInputParam.isIntegral()) return false;
	iOutputParam = static_cast<uint8_t>(stInputParam.asUInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, int16_t& iOutputParam)
{
	if (!stInputParam.isIntegral()) return false;
	iOutputParam = static_cast<int16_t>(stInputParam.asInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, uint16_t& iOutputParam)
{
	if (!stInputParam.isIntegral()) return false;
	iOutputParam = static_cast<uint16_t>(stInputParam.asUInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, int32_t& iOutputParam)
{
	if (!stInputParam.isIntegral()) return false;
	iOutputParam = static_cast<int32_t>(stInputParam.asInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, uint32_t& iOutputParam)
{
	if (!stInputParam.isIntegral()) return false;
	iOutputParam = static_cast<uint32_t>(stInputParam.asUInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, int64_t& iOutputParam)
{
	if (!stInputParam.isIntegral()) return false;
	iOutputParam = static_cast<int64_t>(stInputParam.asInt64());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInputParam, uint64_t& iOutputParam)
{
	if (!stInputParam.isUInt64() && !stInputParam.isIntegral()) return false;
	iOutputParam = static_cast<uint64_t>(stInputParam.asUInt64());
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
	stOutputParam.clear();
	for (Json::ArrayIndex iIndexLocal = 0; iIndexLocal < stInputParam.size(); ++iIndexLocal)
	{
		T stElem{};
		if (!FromJsonValue(stInputParam[iIndexLocal], stElem)) return false;
		stOutputParam.push_back(std::move(stElem));
	}
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
	stOutputParam.clear();
	for (auto it = stInputParam.begin(); it != stInputParam.end(); ++it)
	{
		V stVal{};
		if (!FromJsonValue(*it, stVal)) return false;
		stOutputParam[it.name()] = std::move(stVal);
	}
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
	stOutputParam.clear();
	for (auto it = stInputParam.begin(); it != stInputParam.end(); ++it)
	{
		char* pEnd = NULL;
		int64_t iKey = std::strtoll(it.name().c_str(), &pEnd, 10);
		if (pEnd == it.name().c_str() || *pEnd != '\0') return false;
		if (iKey < INT32_MIN || iKey > INT32_MAX) return false;
		V stVal{};
		if (!FromJsonValue(*it, stVal)) return false;
		stOutputParam[static_cast<int32_t>(iKey)] = std::move(stVal);
	}
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
	stOutputParam.clear();
	for (auto it = stInputParam.begin(); it != stInputParam.end(); ++it)
	{
		const std::string& sNameLocal = it.name();
		if (sNameLocal.empty() || sNameLocal[0] == '-') return false;
		char* pEnd = NULL;
		errno = 0;
		unsigned long long iKey = std::strtoull(sNameLocal.c_str(), &pEnd, 10);
		if (pEnd == sNameLocal.c_str() || *pEnd != '\0' || errno == ERANGE) return false;
		if (iKey > static_cast<unsigned long long>(static_cast<U>(-1))) return false;
		V stVal{};
		if (!FromJsonValue(*it, stVal)) return false;
		stOutputParam[static_cast<U>(iKey)] = std::move(stVal);
	}
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
