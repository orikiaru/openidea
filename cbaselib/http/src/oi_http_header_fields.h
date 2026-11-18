#pragma once
#include "oi_http_message.h"
#include <algorithm>

namespace OI::Http
{
namespace
{
class HeaderFields
{
	static bool ValidValue(std::string_view sValue)
	{
		return std::none_of(sValue.begin(), sValue.end(), [](unsigned char iByte)
		{
			return (iByte < 32 && iByte != 9) || iByte == 127;
		});
	}

  public:
	static bool NameEqual(std::string_view sLeft, std::string_view sRight)
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

	static bool ValidForSend(const Message& stMessage)
	{
		if (!ValidValue(stMessage.sContentType))
		{
			return false;
		}
		for (const auto& [sName, sValue] : stMessage.vHeaders)
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
				if (NameEqual(sName, pReserved))
				{
					return false;
				}
			}
		}
		return true;
	}
};
}
}
