#include "oi_http_message.h"
#include "oi_http_header_fields.h"
#include <charconv>

namespace OI::Http
{
namespace
{
using View = std::string_view;

class CookieField
{
	View m_sLine;

	static View Trim(View sValue)
	{
		const auto iFirst = sValue.find_first_not_of(" \t");
		if (iFirst == View::npos)
		{
			return View{};
		}
		return sValue.substr(iFirst, sValue.find_last_not_of(" \t") - iFirst + 1);
	}

	View TakePart()
	{
		const auto iEnd = m_sLine.find(';');
		const auto sPart = Trim(m_sLine.substr(0, iEnd));
		m_sLine = iEnd == View::npos ? View{} : m_sLine.substr(iEnd + 1);
		return sPart;
	}

  public:
	explicit CookieField(View sLine)
	    : m_sLine(sLine)
	{
	}

	bool Empty() const
	{
		return m_sLine.empty();
	}

	static bool AlphaNumeric(unsigned char iByte)
	{
		return (iByte >= 'a' && iByte <= 'z') || (iByte >= 'A' && iByte <= 'Z') || (iByte >= '0' && iByte <= '9');
	}

	static bool ValidPair(View sName, View sValue)
	{
		return !sName.empty() && std::all_of(sName.begin(), sName.end(), [](unsigned char iByte)
		{
			return AlphaNumeric(iByte) || View("!#$%&'*+-.^_`|~").find(iByte) != View::npos;
		}) && std::all_of(sValue.begin(), sValue.end(), [](unsigned char iByte)
		{
			return iByte >= 0x21 && iByte <= 0x7e && iByte != '"' && iByte != ',' && iByte != ';' && iByte != '\\';
		});
	}

	static bool ValidAttribute(View sValue)
	{
		return std::all_of(sValue.begin(), sValue.end(), [](unsigned char iByte)
		{
			return iByte >= 0x20 && iByte < 0x7f && iByte != ';';
		});
	}

	static bool SameSite(View sValue)
	{
		return HeaderFields::NameEqual(sValue, "Lax") || HeaderFields::NameEqual(sValue, "Strict") || HeaderFields::NameEqual(sValue, "None");
	}

	static void AppendAttribute(std::string& sLine, const char* pName, const std::string& sValue)
	{
		if (!sValue.empty())
		{
			sLine += "; ";
			sLine += pName;
			sLine += "=" + sValue;
		}
	}

	bool ReadPair(std::string& sName, std::string& sValue)
	{
		const View sPair = TakePart();
		const auto iEquals = sPair.find('=');
		if (iEquals == View::npos)
		{
			return false;
		}
		auto sKey = Trim(sPair.substr(0, iEquals));
		auto sRaw = Trim(sPair.substr(iEquals + 1));
		if (sRaw.size() >= 2 && sRaw.front() == '"' && sRaw.back() == '"')
		{
			sRaw = sRaw.substr(1, sRaw.size() - 2);
		}
		if (!ValidPair(sKey, sRaw))
		{
			return false;
		}
		sName = sKey;
		sValue = sRaw;
		return true;
	}

	bool ReadAttribute(Cookie& stCookie)
	{
		const View sPart = TakePart();
		const auto iEquals = sPart.find('=');
		const auto sKey = Trim(sPart.substr(0, iEquals));
		const auto sValue = iEquals == View::npos ? View{} : Trim(sPart.substr(iEquals + 1));
		if (!ValidAttribute(sValue))
		{
			return false;
		}
		if (HeaderFields::NameEqual(sKey, "domain"))
		{
			stCookie.sDomain = sValue;
		}
		else if (HeaderFields::NameEqual(sKey, "path"))
		{
			stCookie.sPath = sValue;
		}
		else if (HeaderFields::NameEqual(sKey, "expires"))
		{
			stCookie.sExpires = sValue;
		}
		else if (HeaderFields::NameEqual(sKey, "max-age"))
		{
			if (sValue.empty())
			{
				return false;
			}
			int64_t iSeconds = 0;
			auto stParsed = std::from_chars(sValue.data(), sValue.data() + sValue.size(), iSeconds);
			if (stParsed.ec != std::errc{} || stParsed.ptr != sValue.data() + sValue.size())
			{
				return false;
			}
			stCookie.iMaxAge = iSeconds;
		}
		else if (HeaderFields::NameEqual(sKey, "secure"))
		{
			stCookie.bSecure = true;
		}
		else if (HeaderFields::NameEqual(sKey, "httponly"))
		{
			stCookie.bHttpOnly = true;
		}
		else if (HeaderFields::NameEqual(sKey, "samesite") && SameSite(sValue))
		{
			stCookie.sSameSite = sValue;
		}
		return true;
	}
};
}

std::string Cookie::EncodeValue(std::string_view sValue)
{
	constexpr char vDigits[] = "0123456789ABCDEF";
	std::string sResult;
	for (unsigned char iByte : sValue)
	{
		if (CookieField::AlphaNumeric(iByte) || View("-._~").find(iByte) != View::npos)
		{
			sResult.push_back(static_cast<char>(iByte));
		}
		else
		{
			sResult.push_back('%');
			sResult.push_back(vDigits[iByte >> 4]);
			sResult.push_back(vDigits[iByte & 15]);
		}
	}
	return sResult;
}

int32_t Cookie::DecodeValue(std::string_view sValue, std::string& sOutput)
{
	std::string sResult;
	for (size_t iIndex = 0; iIndex < sValue.size(); ++iIndex)
	{
		if (sValue[iIndex] != '%')
		{
			sResult.push_back(sValue[iIndex]);
			continue;
		}
		if (sValue.size() - iIndex < 3)
		{
			return OI_ERR_INVALID_ARGUMENT;
		}
		const char* pEnd = sValue.data() + iIndex + 3;
		unsigned iByte = 0;
		auto stParsed = std::from_chars(pEnd - 2, pEnd, iByte, 16);
		if (stParsed.ec != std::errc{} || stParsed.ptr != pEnd)
		{
			return OI_ERR_INVALID_ARGUMENT;
		}
		sResult.push_back(static_cast<char>(iByte));
		iIndex += 2;
	}
	sOutput = std::move(sResult);
	return OI_OK;
}

int32_t Request::AddCookie(std::string_view sName, std::string_view sValue)
{
	if (!CookieField::ValidPair(sName, sValue))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sPair = std::string(sName) + "=" + std::string(sValue);
	for (auto it = vHeaders.rbegin(); it != vHeaders.rend(); ++it)
	{
		if (HeaderFields::NameEqual(it->first, "cookie"))
		{
			it->second += it->second.empty() ? sPair : "; " + sPair;
			return OI_OK;
		}
	}
	vHeaders.emplace_back("Cookie", std::move(sPair));
	return OI_OK;
}

int32_t Request::GetCookies(std::vector<std::pair<std::string, std::string>>& vOutput) const
{
	std::vector<std::pair<std::string, std::string>> vResult;
	for (const auto& [sName, sValue] : vHeaders)
	{
		if (HeaderFields::NameEqual(sName, "cookie"))
		{
			CookieField stField(sValue);
			while (!stField.Empty())
			{
				auto& [sCookieName, sCookieValue] = vResult.emplace_back();
				if (!stField.ReadPair(sCookieName, sCookieValue))
				{
					return OI_ERR_PROTOCOL;
				}
			}
		}
	}
	vOutput = std::move(vResult);
	return OI_OK;
}

int32_t Response::AddCookie(const Cookie& stCookie)
{
	if (!CookieField::ValidPair(stCookie.sName, stCookie.sValue) || !CookieField::ValidAttribute(stCookie.sDomain) ||
	    !CookieField::ValidAttribute(stCookie.sPath) || !CookieField::ValidAttribute(stCookie.sExpires) ||
	    (!stCookie.sSameSite.empty() && !CookieField::SameSite(stCookie.sSameSite)))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sLine = stCookie.sName + "=" + stCookie.sValue;
	CookieField::AppendAttribute(sLine, "Domain", stCookie.sDomain);
	CookieField::AppendAttribute(sLine, "Path", stCookie.sPath);
	CookieField::AppendAttribute(sLine, "Expires", stCookie.sExpires);
	if (stCookie.iMaxAge)
	{
		CookieField::AppendAttribute(sLine, "Max-Age", std::to_string(*stCookie.iMaxAge));
	}
	if (stCookie.bSecure)
	{
		sLine += "; Secure";
	}
	if (stCookie.bHttpOnly)
	{
		sLine += "; HttpOnly";
	}
	CookieField::AppendAttribute(sLine, "SameSite", stCookie.sSameSite);
	vHeaders.emplace_back("Set-Cookie", std::move(sLine));
	return OI_OK;
}

int32_t Response::GetCookies(std::vector<Cookie>& vOutput) const
{
	std::vector<Cookie> vResult;
	for (const auto& [sName, sValue] : vHeaders)
	{
		if (HeaderFields::NameEqual(sName, "set-cookie"))
		{
			CookieField stField(sValue);
			auto& stCookie = vResult.emplace_back();
			if (!stField.ReadPair(stCookie.sName, stCookie.sValue))
			{
				return OI_ERR_PROTOCOL;
			}
			while (!stField.Empty())
			{
				if (!stField.ReadAttribute(stCookie))
				{
					return OI_ERR_PROTOCOL;
				}
			}
		}
	}
	vOutput = std::move(vResult);
	return OI_OK;
}
}
