#include "oi_file.h"
#include <filesystem>
#include <system_error>

namespace OI
{
namespace File
{
bool Exists(const std::string& sPath)
{
	if (sPath.empty())
	{
		return false;
	}
	std::error_code stEc;
	return std::filesystem::exists(sPath, stEc) && !stEc;
}

bool MakeDirs(const std::string& sPath)
{
	if (sPath.empty())
	{
		return false;
	}
	std::error_code stEc;
	if (std::filesystem::is_directory(sPath, stEc))
	{
		return true;
	}
	std::filesystem::create_directories(sPath, stEc);
	if (stEc)
	{
		return false;
	}
	return std::filesystem::is_directory(sPath, stEc);
}
}
}
