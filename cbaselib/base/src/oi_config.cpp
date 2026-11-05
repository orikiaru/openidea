#include "cpptoml.h"
#include "oi_config.h"
#include <iostream>

namespace OI
{
namespace Base
{
Config::Config()
{
	m_pTomlConfig = cpptoml::make_table();
}

bool Config::Parse(const std::string& sConfFileName)
{
	std::shared_ptr<cpptoml::table> pConfig;
	try
	{
		pConfig = cpptoml::parse_file(sConfFileName);
	}
	catch (const cpptoml::parse_exception& stErrorLocal)
	{
		std::cerr << "Failed to parse " << sConfFileName << ": " << stErrorLocal.what() << std::endl;
		return false;
	}
	for (const auto& [sName, pValue] : *pConfig)
	{
		if (m_pTomlConfig->contains(sName))
		{
			//TODO LOGWarning
		}
		m_pTomlConfig->insert(sName, pValue);
	}
	return true;
}
}
}
