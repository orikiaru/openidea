#pragma once
#include "oi_errno.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace OI::Http
{
using Headers = std::vector<std::pair<std::string, std::string>>;

inline bool HeaderNameEqual(std::string_view sLeft, std::string_view sRight)
{
	auto fnLower = [](char iChar)
	{
		return iChar >= 'A' && iChar <= 'Z' ? iChar + ('a' - 'A') : static_cast<int>(iChar);
	};
	return sLeft.size() == sRight.size() && std::equal(sLeft.begin(), sLeft.end(), sRight.begin(), [fnLower](char iLeft, char iRight)
	{
		return fnLower(iLeft) == fnLower(iRight);
	});
}

struct Message
{
	Headers vHeaders;
	std::string sBody;
	std::string sContentType;

	const std::string* GetHeader(std::string_view sName) const
	{
		for (const auto& [sKey, sValue] : vHeaders)
		{
			if (HeaderNameEqual(sKey, sName))
			{
				return &sValue;
			}
		}
		return nullptr;
	}
};

struct Request : Message
{
	std::string sMethod = "GET";
	std::string sPath;
	std::map<std::string, std::string> mQuery;
};

struct Response : Message
{
	int32_t iStatus = 0;
	int32_t iCode = OI_ERR_CONNECT;
	std::string sError;
};

namespace Detail
{
inline bool ValidValue(std::string_view sValue)
{
	return std::none_of(sValue.begin(), sValue.end(), [](unsigned char iByte)
	{
		return (iByte < 32 && iByte != 9) || iByte == 127;
	});
}

inline bool ValidExtraHeaders(const Headers& vHeaders)
{
	for (const auto& [sName, sValue] : vHeaders)
	{
		if (sName.empty() || !ValidValue(sValue) || !std::all_of(sName.begin(), sName.end(), [](unsigned char iByte)
		{
			return (iByte >= 'A' && iByte <= 'Z') || (iByte >= 'a' && iByte <= 'z') || (iByte >= '0' && iByte <= '9') ||
			       std::string_view("!#$%&'*+-.^_`|~").find(iByte) != std::string_view::npos;
		}))
		{
			return false;
		}
		for (const auto* pReserved : {"host", "connection", "content-length", "transfer-encoding", "content-type", "trailer", "upgrade", "expect"})
		{
			if (HeaderNameEqual(sName, pReserved))
			{
				return false;
			}
		}
	}
	return true;
}
}
}
