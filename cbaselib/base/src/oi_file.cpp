#include "oi_file.h"
#include <filesystem>
#include <system_error>

namespace OI { namespace File {
bool Exists(const std::string& sPathParam)
{
	if (sPathParam.empty())
	{
		return false;
	}
	std::error_code stEc;
	return std::filesystem::exists(sPathParam, stEc) && !stEc;
}

bool MakeDirs(const std::string& sPathParam)
{
	if (sPathParam.empty())
	{
		return false;
	}
	std::error_code stEc;
	if (std::filesystem::is_directory(sPathParam, stEc))
	{
		return true;
	}
	std::filesystem::create_directories(sPathParam, stEc);
	if (stEc)
	{
		return false;
	}
	return std::filesystem::is_directory(sPathParam, stEc);
}
}}
