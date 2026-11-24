#pragma once
#include "oi_errno.h"
#include "cpptoml.h"
#include "oi_singleton.h"
#include <assert.h>
#include <memory>
#include <utility>
#include <cstdio>
namespace cpptoml
{
class table;
}

namespace OI
{
namespace Base
{
// 进程唯一的 toml。多次 Parse 把新文件合进已有表，同名顶层键后写的覆盖先写的。
class Config
{
	DECLARE_SINGLETON(Config)
	Config();
	// Parse 成功后合进来的表。读接口都从这里取。
	std::shared_ptr<cpptoml::table> m_pTomlConfig;

  public:
	// 读一份 toml 并合进当前表。解析失败返回 OI_ERR_INVALID_ARGUMENT，已有内容不动。
	int32_t Parse(const std::string& sConfFileName);

	bool Contains(const std::string& sKey) const
	{
		return m_pTomlConfig->contains_qualified(sKey);
	}

	// 按点分键取单个值。没有这个键或类型对不上时返回 false，不改输出。
	template <class T>
	bool GetAs(const std::string& sKey, T& stValueLocal) const
	{
		if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool>)
		{
			auto stInteger = m_pTomlConfig->get_qualified_as<int64_t>(sKey);
			if (!stInteger)
			{
				return false;
			}
			if (!std::in_range<T>(*stInteger))
			{
				fprintf(stderr, "configuration integer out of range: %s\n", sKey.c_str());
				return false;
			}
			stValueLocal = static_cast<T>(*stInteger);
			return true;
		}
		cpptoml::option<T> stValue;
		stValue = m_pTomlConfig->get_qualified_as<T>(sKey);
		if (!stValue)
		{
			return false;
		}
		stValueLocal = *stValue;
		return true;
	}

	// 按点分键取数组。没有这个键或元素类型对不上时返回 false，不改输出。
	template <class T>
	bool GetArrayAs(const std::string& sKey, typename std::vector<T>& stValueLocal) const
	{
		if constexpr (std::is_integral_v<T> && !std::is_same_v<T, bool> && !std::is_same_v<T, int64_t>)
		{
			auto stIntegers = m_pTomlConfig->get_qualified_array_of<int64_t>(sKey);
			if (!stIntegers)
			{
				return false;
			}
			std::vector<T> vOutput;
			for (auto iValue : *stIntegers)
			{
				if (!std::in_range<T>(iValue))
				{
					fprintf(stderr, "configuration integer array out of range: %s\n", sKey.c_str());
					return false;
				}
				vOutput.push_back(static_cast<T>(iValue));
			}
			stValueLocal = std::move(vOutput);
			return true;
		}
		else
		{
			typename cpptoml::option<typename std::vector<T>> stValue;
			stValue = m_pTomlConfig->get_qualified_array_of<T>(sKey);
			if (!stValue)
			{
				return false;
			}
			stValueLocal = *stValue;
			return true;
		}
	}

	// 进程唯一配置，给读取用。
	static const Config& GlobalConfig()
	{
		return GetInstance();
	}
	// 同一个实例，给 Parse 用。
	static Config& GlobalMutableConfig()
	{
		return GetInstance();
	}
};

namespace Detail
{
// 限定提前返回宏只能把读取失败作为 bool 返回，避免错误码接口误报成功。
struct ConfigReadFailure
{
	constexpr operator bool() const
	{
		return false;
	}
	template <class T>
	operator T() const = delete;
};
}
}
}

#define GetConfigAndAssert(KEY, VALUE)                             \
	do                                                             \
	{                                                              \
		if (!::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE)) \
		{                                                          \
			assert(false && "GetConfigAndAssert: " #KEY);           \
		}                                                          \
	} while (false)
#define TryGetConfigNoReturn(KEY, VALUE)                      \
	{                                                         \
		::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE); \
	}
#define TryGetConfigAndReturnFalse(KEY, VALUE)                     \
	do                                                             \
	{                                                              \
		if (!::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE)) \
		{                                                          \
			return ::OI::Base::Detail::ConfigReadFailure{};         \
		}                                                          \
	} while (false)
//可以拿取返回值
#define TryGetConfig(KEY, VALUE) ::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE)
#define ParseConfig(PATH) ::OI::Base::Config::GlobalMutableConfig().Parse(PATH)
