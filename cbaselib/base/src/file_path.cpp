#include "file_path.h"
#include <filesystem>
#include <system_error>

namespace OI { namespace File {
bool Exists(const std::string& path)
{
	if (path.empty())
	{
		return false;
	}
	std::error_code ec;
	return std::filesystem::exists(path, ec) && !ec;
}

bool MakeDirs(const std::string& path)
{
	if (path.empty())
	{
		return false;
	}
	std::error_code ec;
	if (std::filesystem::is_directory(path, ec))
	{
		return true;
	}
	std::filesystem::create_directories(path, ec);
	return !ec && std::filesystem::is_directory(path);
}
}}
