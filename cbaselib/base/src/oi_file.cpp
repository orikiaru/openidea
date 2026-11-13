#include "oi_file.h"
#include <filesystem>
#include <system_error>
#include <cstdio>
#include <cerrno>

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

int32_t MakeDirs(const std::string& sPath)
{
	if (sPath.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::error_code stError;
	if (std::filesystem::is_directory(sPath, stError))
	{
		return OI_OK;
	}
	stError.clear();
	std::filesystem::create_directories(sPath, stError);
	if (!stError && std::filesystem::is_directory(sPath, stError))
	{
		return OI_OK;
	}
	const int32_t iCode = stError ? stError.value() : ENOTDIR;
	fprintf(stderr, "MakeDirs failed path=%s code=%d reason=%s\n", sPath.c_str(), iCode,
	    stError ? stError.message().c_str() : "not a directory");
	return iCode;
}
}
}
