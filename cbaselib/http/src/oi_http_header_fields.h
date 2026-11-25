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

	class ConnectionTokens
	{
		bool m_bClose = false;
		bool m_bKeepAlive = false;

	  public:
		void Read(std::string_view sValue)
		{
			while (!sValue.empty())
			{
				const auto iComma = sValue.find(',');
				const auto sToken = sValue.substr(0, iComma);
				const auto iFirst = sToken.find_first_not_of(" \t");
				if (iFirst != sToken.npos)
				{
					const auto sName = sToken.substr(iFirst, sToken.find_last_not_of(" \t") - iFirst + 1);
					m_bClose |= NameEqual(sName, "close");
					m_bKeepAlive |= NameEqual(sName, "keep-alive");
				}
				if (iComma == sValue.npos)
				{
					break;
				}
				sValue.remove_prefix(iComma + 1);
			}
		}

		bool ShouldClose(bool bHttp10) const
		{
			return m_bClose || (bHttp10 && !m_bKeepAlive);
		}
	};

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
