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

namespace OI
{
namespace Rpc
{

inline void ToJsonValue(Json::Value& stOutput, bool bInput)
{
	stOutput = bInput;
}
inline void ToJsonValue(Json::Value& stOutput, int8_t iInput)
{
	stOutput = static_cast<int32_t>(iInput);
}
inline void ToJsonValue(Json::Value& stOutput, uint8_t iInput)
{
	stOutput = static_cast<uint32_t>(iInput);
}
inline void ToJsonValue(Json::Value& stOutput, int16_t iInput)
{
	stOutput = static_cast<int32_t>(iInput);
}
inline void ToJsonValue(Json::Value& stOutput, uint16_t iInput)
{
	stOutput = static_cast<uint32_t>(iInput);
}
inline void ToJsonValue(Json::Value& stOutput, int32_t iInput)
{
	stOutput = static_cast<Json::Int>(iInput);
}
inline void ToJsonValue(Json::Value& stOutput, uint32_t iInput)
{
	stOutput = static_cast<Json::UInt>(iInput);
}
inline void ToJsonValue(Json::Value& stOutput, int64_t iInput)
{
	stOutput = static_cast<Json::Int64>(iInput);
}
inline void ToJsonValue(Json::Value& stOutput, uint64_t iInput)
{
	stOutput = static_cast<Json::UInt64>(iInput);
}
inline void ToJsonValue(Json::Value& stOutput, float fInput)
{
	stOutput = static_cast<double>(fInput);
}
inline void ToJsonValue(Json::Value& stOutput, double dInput)
{
	stOutput = dInput;
}
inline void ToJsonValue(Json::Value& stOutput, const std::string& sInput)
{
	stOutput = sInput;
}
inline void ToJsonValue(Json::Value& stOutput, const ::OI::Base::Bytes& stInput)
{
	stOutput = std::string(stInput.Data(), stInput.Data() ? stInput.Size() : 0);
}

inline bool FromJsonValue(const Json::Value& stInput, bool& bOutput)
{
	if (!stInput.isBool())
	{
		return false;
	}
	bOutput = stInput.asBool();
	return true;
}
// 只接受整数 token，先检查目标范围，避免截断和有符号/无符号溢出。
template <typename T>
std::enable_if_t<std::is_integral_v<T> && !std::is_same_v<T, bool>, bool>
FromJsonValue(const Json::Value& stInput, T& stOutput)
{
	if (stInput.type() == Json::intValue)
	{
		const auto iValue = stInput.asInt64();
		if constexpr (std::is_signed_v<T>)
		{
			if (iValue < (std::numeric_limits<T>::min)() || iValue > (std::numeric_limits<T>::max)())
			{
				return false;
			}
		}
		else if (iValue < 0 || static_cast<Json::UInt64>(iValue) > (std::numeric_limits<T>::max)())
		{
			return false;
		}
		stOutput = static_cast<T>(iValue);
		return true;
	}
	if (stInput.type() == Json::uintValue)
	{
		const auto iValue = stInput.asUInt64();
		if (iValue > static_cast<Json::UInt64>((std::numeric_limits<T>::max)()))
		{
			return false;
		}
		stOutput = static_cast<T>(iValue);
		return true;
	}
	return false;
}

// 在字符串有效期内完整解析整数键，不依赖 errno 或区域设置。
template <typename T>
bool ParseJsonIntegerKey(const std::string& sKey, T& stOutput)
{
	T stValue{};
	const auto stParsed = std::from_chars(sKey.data(), sKey.data() + sKey.size(), stValue);
	if (stParsed.ec != std::errc{} || stParsed.ptr != sKey.data() + sKey.size())
	{
		return false;
	}
	stOutput = stValue;
	return true;
}

inline bool FromJsonValue(const Json::Value& stInput, float& fOutput)
{
	if (!stInput.isNumeric())
	{
		return false;
	}
	fOutput = static_cast<float>(stInput.asDouble());
	return true;
}
inline bool FromJsonValue(const Json::Value& stInput, double& dOutput)
{
	if (!stInput.isNumeric())
	{
		return false;
	}
	dOutput = stInput.asDouble();
	return true;
}
inline bool FromJsonValue(const Json::Value& stInput, std::string& sOutput)
{
	if (!stInput.isString())
	{
		return false;
	}
	sOutput = stInput.asString();
	return true;
}
inline bool FromJsonValue(const Json::Value& stInput, ::OI::Base::Bytes& stOutput)
{
	if (!stInput.isString())
	{
		return false;
	}
	const std::string sText = stInput.asString();
	stOutput.Reserve(0);
	stOutput.Append(sText.data(), sText.size());
	return true;
}

template <typename T>
bool FromJsonValue(const Json::Value& stInput, std::vector<T>& stOutput)
{
	if (!stInput.isArray())
	{
		return false;
	}
	std::vector<T> vDecoded;
	vDecoded.reserve(stInput.size());
	for (Json::ArrayIndex iIndexLocal = 0; iIndexLocal < stInput.size(); ++iIndexLocal)
	{
		T stElem{};
		if (!FromJsonValue(stInput[iIndexLocal], stElem))
		{
			return false;
		}
		vDecoded.push_back(std::move(stElem));
	}
	stOutput.swap(vDecoded);
	return true;
}

template <typename T>
void ToJsonValue(Json::Value& stOutput, const std::vector<T>& stInput)
{
	stOutput = Json::Value(Json::arrayValue);
	for (size_t iIndexLocal = 0; iIndexLocal < stInput.size(); ++iIndexLocal)
	{
		Json::Value stChild;
		ToJsonValue(stChild, stInput[iIndexLocal]);
		stOutput.append(std::move(stChild));
	}
}

template <typename V>
bool FromJsonValue(const Json::Value& stInput, std::map<std::string, V>& stOutput)
{
	if (!stInput.isObject())
	{
		return false;
	}
	std::map<std::string, V> mDecoded;
	for (auto it = stInput.begin(); it != stInput.end(); ++it)
	{
		V stVal{};
		if (!FromJsonValue(*it, stVal))
		{
			return false;
		}
		mDecoded[it.name()] = std::move(stVal);
	}
	stOutput.swap(mDecoded);
	return true;
}

template <typename V>
void ToJsonValue(Json::Value& stOutput, const std::map<std::string, V>& stInput)
{
	stOutput = Json::Value(Json::objectValue);
	for (const auto& [sKey, stValue] : stInput)
	{
		Json::Value stChild;
		ToJsonValue(stChild, stValue);
		stOutput[sKey] = std::move(stChild);
	}
}

template <typename V>
bool FromJsonValue(const Json::Value& stInput, std::map<int32_t, V>& stOutput)
{
	if (!stInput.isObject())
	{
		return false;
	}
	std::map<int32_t, V> mDecoded;
	for (auto it = stInput.begin(); it != stInput.end(); ++it)
	{
		int32_t iKey{};
		if (!ParseJsonIntegerKey(it.name(), iKey))
		{
			return false;
		}
		V stVal{};
		if (!FromJsonValue(*it, stVal))
		{
			return false;
		}
		mDecoded[iKey] = std::move(stVal);
	}
	stOutput.swap(mDecoded);
	return true;
}

template <typename V>
void ToJsonValue(Json::Value& stOutput, const std::map<int32_t, V>& stInput)
{
	stOutput = Json::Value(Json::objectValue);
	for (const auto& [iKey, stValue] : stInput)
	{
		Json::Value stChild;
		ToJsonValue(stChild, stValue);
		stOutput[std::to_string(iKey)] = std::move(stChild);
	}
}

template <typename U, typename V>
bool FromJsonUnsignedKeyMap(const Json::Value& stInput, std::map<U, V>& stOutput)
{
	if (!stInput.isObject())
	{
		return false;
	}
	std::map<U, V> mDecoded;
	for (auto it = stInput.begin(); it != stInput.end(); ++it)
	{
		U stKey{};
		if (!ParseJsonIntegerKey(it.name(), stKey))
		{
			return false;
		}
		V stVal{};
		if (!FromJsonValue(*it, stVal))
		{
			return false;
		}
		mDecoded[stKey] = std::move(stVal);
	}
	stOutput.swap(mDecoded);
	return true;
}

template <typename U, typename V>
void ToJsonUnsignedKeyMap(Json::Value& stOutput, const std::map<U, V>& stInput)
{
	stOutput = Json::Value(Json::objectValue);
	for (const auto& [stKey, stValue] : stInput)
	{
		Json::Value stChild;
		ToJsonValue(stChild, stValue);
		stOutput[std::to_string(stKey)] = std::move(stChild);
	}
}

template <typename V>
bool FromJsonValue(const Json::Value& stInput, std::map<uint32_t, V>& stOutput)
{
	return FromJsonUnsignedKeyMap(stInput, stOutput);
}

template <typename V>
void ToJsonValue(Json::Value& stOutput, const std::map<uint32_t, V>& stInput)
{
	ToJsonUnsignedKeyMap(stOutput, stInput);
}

template <typename V>
bool FromJsonValue(const Json::Value& stInput, std::map<uint64_t, V>& stOutput)
{
	return FromJsonUnsignedKeyMap(stInput, stOutput);
}

template <typename V>
void ToJsonValue(Json::Value& stOutput, const std::map<uint64_t, V>& stInput)
{
	ToJsonUnsignedKeyMap(stOutput, stInput);
}

template <typename T>
auto FromJsonValue(const Json::Value& stInput, T& stOutput) -> decltype(stOutput.FromJson(stInput))
{
	if (!stInput.isObject())
	{
		return false;
	}
	return stOutput.FromJson(stInput);
}

template <typename T>
auto ToJsonValue(Json::Value& stOutput, const T& stInput) -> decltype(stInput.ToJson(stOutput))
{
	stInput.ToJson(stOutput);
}

}
}
