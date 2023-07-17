#include "cpptoml.h"
#include "config.h"
#include <iostream>

Config::Config()
{
	_toml_config = cpptoml::make_table();
}

bool Config::Parse(const std::string& conf_file_name)
{
	std::shared_ptr<cpptoml::table> config;
	try
	{
		config = cpptoml::parse_file(conf_file_name);
	}
	catch (const cpptoml::parse_exception & e)
	{
		std::cerr << "Failed to parse " << conf_file_name << ": " << e.what() << std::endl;
		return false;
	}
	for (auto it = config->begin(); it != config->end(); ++it)
	{
		if (_toml_config->contains(it->first))
		{
			//TODO LOGWarning
		}
		_toml_config->insert(it->first, it->second);
	}
	return true;
}

const Config& Config::GlobalConfig()
{
	return GlobalMutableConfig();
}

Config& Config::GlobalMutableConfig()
{
	static Config inst;
	return inst;
}
