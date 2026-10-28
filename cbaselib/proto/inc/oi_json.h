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

inline void ToJsonValue(Json::Value& out, bool in) { out = in; }
inline void ToJsonValue(Json::Value& out, int8_t in) { out = static_cast<int32_t>(in); }
inline void ToJsonValue(Json::Value& out, uint8_t in) { out = static_cast<uint32_t>(in); }
inline void ToJsonValue(Json::Value& out, int16_t in) { out = static_cast<int32_t>(in); }
inline void ToJsonValue(Json::Value& out, uint16_t in) { out = static_cast<uint32_t>(in); }
inline void ToJsonValue(Json::Value& out, int32_t in) { out = static_cast<Json::Int>(in); }
inline void ToJsonValue(Json::Value& out, uint32_t in) { out = static_cast<Json::UInt>(in); }
inline void ToJsonValue(Json::Value& out, int64_t in) { out = static_cast<Json::Int64>(in); }
inline void ToJsonValue(Json::Value& out, uint64_t in) { out = static_cast<Json::UInt64>(in); }
inline void ToJsonValue(Json::Value& out, float in) { out = static_cast<double>(in); }
inline void ToJsonValue(Json::Value& out, double in) { out = in; }
inline void ToJsonValue(Json::Value& out, const std::string& in) { out = in; }
inline void ToJsonValue(Json::Value& out, const ::OI::Base::Bytes& in)
{
	out = std::string(in.Data(), in.Data() ? in.Size() : 0);
}

inline bool FromJsonValue(const Json::Value& in, bool& out)
{
	if (!in.isBool()) return false;
	out = in.asBool();
	return true;
}
inline bool FromJsonValue(const Json::Value& in, int8_t& out)
{
	if (!in.isIntegral()) return false;
	out = static_cast<int8_t>(in.asInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, uint8_t& out)
{
	if (!in.isIntegral()) return false;
	out = static_cast<uint8_t>(in.asUInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, int16_t& out)
{
	if (!in.isIntegral()) return false;
	out = static_cast<int16_t>(in.asInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, uint16_t& out)
{
	if (!in.isIntegral()) return false;
	out = static_cast<uint16_t>(in.asUInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, int32_t& out)
{
	if (!in.isIntegral()) return false;
	out = static_cast<int32_t>(in.asInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, uint32_t& out)
{
	if (!in.isIntegral()) return false;
	out = static_cast<uint32_t>(in.asUInt());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, int64_t& out)
{
	if (!in.isIntegral()) return false;
	out = static_cast<int64_t>(in.asInt64());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, uint64_t& out)
{
	if (!in.isUInt64() && !in.isIntegral()) return false;
	out = static_cast<uint64_t>(in.asUInt64());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, float& out)
{
	if (!in.isNumeric()) return false;
	out = static_cast<float>(in.asDouble());
	return true;
}
inline bool FromJsonValue(const Json::Value& in, double& out)
{
	if (!in.isNumeric()) return false;
	out = in.asDouble();
	return true;
}
inline bool FromJsonValue(const Json::Value& in, std::string& out)
{
	if (!in.isString()) return false;
	out = in.asString();
	return true;
}
inline bool FromJsonValue(const Json::Value& in, ::OI::Base::Bytes& out)
{
	if (!in.isString()) return false;
	const std::string s = in.asString();
	out.Reserve(0);
	out.Append(s.data(), s.size());
	return true;
}

template<typename T>
bool FromJsonValue(const Json::Value& in, std::vector<T>& out)
{
	if (!in.isArray()) return false;
	out.clear();
	for (Json::ArrayIndex i = 0; i < in.size(); ++i)
	{
		T elem{};
		if (!FromJsonValue(in[i], elem)) return false;
		out.push_back(std::move(elem));
	}
	return true;
}

template<typename T>
void ToJsonValue(Json::Value& out, const std::vector<T>& in)
{
	out = Json::Value(Json::arrayValue);
	for (size_t i = 0; i < in.size(); ++i)
	{
		Json::Value child;
		ToJsonValue(child, in[i]);
		out.append(std::move(child));
	}
}

template<typename V>
bool FromJsonValue(const Json::Value& in, std::map<std::string, V>& out)
{
	if (!in.isObject()) return false;
	out.clear();
	for (Json::Value::const_iterator it = in.begin(); it != in.end(); ++it)
	{
		V val{};
		if (!FromJsonValue(*it, val)) return false;
		out[it.name()] = std::move(val);
	}
	return true;
}

template<typename V>
void ToJsonValue(Json::Value& out, const std::map<std::string, V>& in)
{
	out = Json::Value(Json::objectValue);
	for (typename std::map<std::string, V>::const_iterator it = in.begin(); it != in.end(); ++it)
	{
		Json::Value child;
		ToJsonValue(child, it->second);
		out[it->first] = std::move(child);
	}
}

template<typename V>
bool FromJsonValue(const Json::Value& in, std::map<int32_t, V>& out)
{
	if (!in.isObject()) return false;
	out.clear();
	for (Json::Value::const_iterator it = in.begin(); it != in.end(); ++it)
	{
		char* end = NULL;
		int64_t key = std::strtoll(it.name().c_str(), &end, 10);
		if (end == it.name().c_str() || *end != '\0') return false;
		if (key < INT32_MIN || key > INT32_MAX) return false;
		V val{};
		if (!FromJsonValue(*it, val)) return false;
		out[static_cast<int32_t>(key)] = std::move(val);
	}
	return true;
}

template<typename V>
void ToJsonValue(Json::Value& out, const std::map<int32_t, V>& in)
{
	out = Json::Value(Json::objectValue);
	for (typename std::map<int32_t, V>::const_iterator it = in.begin(); it != in.end(); ++it)
	{
		Json::Value child;
		ToJsonValue(child, it->second);
		out[std::to_string(it->first)] = std::move(child);
	}
}

template<typename U, typename V>
bool FromJsonUnsignedKeyMap(const Json::Value& in, std::map<U, V>& out)
{
	if (!in.isObject()) return false;
	out.clear();
	for (Json::Value::const_iterator it = in.begin(); it != in.end(); ++it)
	{
		const std::string& name = it.name();
		if (name.empty() || name[0] == '-') return false;
		char* end = NULL;
		errno = 0;
		unsigned long long key = std::strtoull(name.c_str(), &end, 10);
		if (end == name.c_str() || *end != '\0' || errno == ERANGE) return false;
		if (key > static_cast<unsigned long long>(static_cast<U>(-1))) return false;
		V val{};
		if (!FromJsonValue(*it, val)) return false;
		out[static_cast<U>(key)] = std::move(val);
	}
	return true;
}

template<typename U, typename V>
void ToJsonUnsignedKeyMap(Json::Value& out, const std::map<U, V>& in)
{
	out = Json::Value(Json::objectValue);
	for (typename std::map<U, V>::const_iterator it = in.begin(); it != in.end(); ++it)
	{
		Json::Value child;
		ToJsonValue(child, it->second);
		out[std::to_string(it->first)] = std::move(child);
	}
}

template<typename V>
bool FromJsonValue(const Json::Value& in, std::map<uint32_t, V>& out)
{
	return FromJsonUnsignedKeyMap(in, out);
}

template<typename V>
void ToJsonValue(Json::Value& out, const std::map<uint32_t, V>& in)
{
	ToJsonUnsignedKeyMap(out, in);
}

template<typename V>
bool FromJsonValue(const Json::Value& in, std::map<uint64_t, V>& out)
{
	return FromJsonUnsignedKeyMap(in, out);
}

template<typename V>
void ToJsonValue(Json::Value& out, const std::map<uint64_t, V>& in)
{
	ToJsonUnsignedKeyMap(out, in);
}

template<typename T>
auto FromJsonValue(const Json::Value& in, T& out) -> decltype(out.FromJson(in))
{
	if (!in.isObject()) return false;
	return out.FromJson(in);
}

template<typename T>
auto ToJsonValue(Json::Value& out, const T& in) -> decltype(in.ToJson(out))
{
	in.ToJson(out);
}

}}
