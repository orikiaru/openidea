#pragma once
#include "cpptoml.h"
#include "oi_singleton.h"
#include <assert.h>
#include <memory>
//配置文件实例
namespace cpptoml
{
class table;
}

//暂时先将Config设计成全局Config并且配置文件为单个toml文件
namespace OI
{
namespace Base
{
class Config
{
	DECLARE_SINGLETON(Config)
	Config();
	std::shared_ptr<cpptoml::table> m_pTomlConfig;

  public:
	bool Parse(const std::string& sConfFileName);

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

	static const Config& GlobalConfig()
	{
		return GetInstance();
	}
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
