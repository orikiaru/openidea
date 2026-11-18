#pragma once
#include "oi_errno.h"
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OI::Http
{
using Headers = std::vector<std::pair<std::string, std::string>>;

struct Message
{
	Headers vHeaders;
	std::string sBody;
	std::string sContentType;

	const std::string* GetHeader(std::string_view sName) const;
};

struct Cookie
{
	std::string sName;
	std::string sValue;
	std::string sDomain;
	std::string sPath;
	std::string sExpires;
	std::optional<int64_t> iMaxAge;
	bool bSecure = false;
	bool bHttpOnly = false;
	std::string sSameSite;

	static std::string EncodeValue(std::string_view sValue);
	static int32_t DecodeValue(std::string_view sValue, std::string& sOutput);
};

struct Request : Message
{
	std::string sMethod = "GET";
	std::string sPath;
	std::map<std::string, std::string> mQuery;
	int32_t AddCookie(std::string_view sName, std::string_view sValue);
	int32_t GetCookies(std::vector<std::pair<std::string, std::string>>& vOutput) const;
};

struct Response : Message
{
	int32_t iStatus = 0;
	int32_t iCode = OI_ERR_CONNECT;
	std::string sError;
	int32_t AddCookie(const Cookie& stCookie);
	int32_t GetCookies(std::vector<Cookie>& vOutput) const;
};

}
