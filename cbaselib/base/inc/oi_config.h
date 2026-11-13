#pragma once
#include "oi_errno.h"
#include "cpptoml.h"
#include "oi_singleton.h"
#include <assert.h>
#include <memory>
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
	// 读一份 toml 并合进当前表。解析失败返回 false，已有内容不动。
	int32_t Parse(const std::string& sConfFileName);

	// 按点分键取单个值。没有这个键或类型对不上时返回 false，不改输出。
	template <class T>
	bool GetAs(const std::string& sKey, T& stValueLocal) const
	{
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
		typename cpptoml::option<typename std::vector<T>> stValue;
		stValue = m_pTomlConfig->get_qualified_array_of<T>(sKey);
		if (!stValue)
		{
			return false;
		}
		stValueLocal = *stValue;
		return true;
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
}
}

#define GetConfigAndAssert(KEY, VALUE)                                \
	{                                                                 \
		assert(::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE)); \
	}
#define TryGetConfigNoReturn(KEY, VALUE)                      \
	{                                                         \
		::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE); \
	}
#define TryGetConfigAndReturnFalse(KEY, VALUE)                     \
	{                                                              \
		if (!::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE)) \
			return false;                                          \
	}
//可以拿取返回值
#define TryGetConfig(KEY, VALUE) ::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE)
#define ParseConfig(PATH) ::OI::Base::Config::GlobalMutableConfig().Parse(PATH)
