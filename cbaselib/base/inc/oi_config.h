#ifndef  __config_h_j__
#define  __config_h_j__	
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
namespace OI { namespace Base {
class Config
{
	DECLARE_SINGLETON(Config)
	Config();
	std::shared_ptr<cpptoml::table> _toml_config;
public:
	bool Parse(const std::string& conf_file_name);
	
	template<class T>
	bool GetAs(const std::string& key, T& t) const
	{
		cpptoml::option<T> v;
		v = _toml_config->get_qualified_as<T>(key);
		if (!v)
		{
			return false;
		}
		t = *v;
		return true;
	}

	template<class T>
	bool GetArrayAs(const std::string& key, typename std::vector<T>& t) const
	{
		typename cpptoml::option<typename std::vector<T>> v;
		v = _toml_config->get_qualified_array_of<T>(key);
		if (!v)
		{
			return false;
		}
		t = *v;
		return true;
	}

	static const Config& GlobalConfig() { return GetInstance(); }
	static Config& GlobalMutableConfig() { return GetInstance(); }
};
}}

#define GetConfigAndAssert(KEY, VALUE)			{ assert(::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE));}
#define TryGetConfigNoReturn(KEY, VALUE)		{ ::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE);}
#define TryGetConfigAndReturnFalse(KEY, VALUE)	{ if(!::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE)) return false; }
//可以拿取返回值
#define TryGetConfig(KEY, VALUE)				::OI::Base::Config::GlobalConfig().GetAs(KEY, VALUE)
#define ParseConfig(PATH)						::OI::Base::Config::GlobalMutableConfig().Parse(PATH)

#endif // ! __config_h_j__
