#include "oi_http_header_fields.h"
#include "json/json.h"
#include <cmath>

namespace OI::Http
{
namespace
{
constexpr size_t kJsonLimit = 8 * 1024 * 1024;

class JsonWire
{
  public:
	static bool ValidUtf8(std::string_view sText)
	{
		size_t iOffset = 0;
		while (iOffset < sText.size())
		{
			const auto iLead = static_cast<unsigned char>(sText[iOffset++]);
			if (iLead < 0x80)
			{
				continue;
			}
			if (iLead < 0xc2 || iLead > 0xf4)
			{
				return false;
			}
			const size_t iCount = iLead < 0xe0 ? 1 : (iLead < 0xf0 ? 2 : 3);
			if (sText.size() - iOffset < iCount)
			{
				return false;
			}
			const auto iFirst = static_cast<unsigned char>(sText[iOffset]);
			if ((iLead == 0xe0 && iFirst < 0xa0) || (iLead == 0xed && iFirst >= 0xa0) ||
			    (iLead == 0xf0 && iFirst < 0x90) || (iLead == 0xf4 && iFirst >= 0x90))
			{
				return false;
			}
			for (size_t iIndex = 0; iIndex < iCount; ++iIndex)
			{
				if ((static_cast<unsigned char>(sText[iOffset++]) & 0xc0) != 0x80)
				{
					return false;
				}
			}
		}
		return true;
	}

	static bool ValidTree(const Json::Value& stValue, size_t iDepth = 1)
	{
		if (iDepth > 256)
		{
			return false;
		}
		if (stValue.isString())
		{
			const char* pBegin = nullptr;
			const char* pEnd = nullptr;
			return stValue.getString(&pBegin, &pEnd) && ValidUtf8({pBegin, static_cast<size_t>(pEnd - pBegin)});
		}
		if (stValue.type() == Json::realValue)
		{
			return std::isfinite(stValue.asDouble());
		}
		for (auto it = stValue.begin(); it != stValue.end(); ++it)
		{
			if (stValue.isObject())
			{
				const char* pEnd = nullptr;
				const char* pBegin = it.memberName(&pEnd);
				if (!ValidUtf8({pBegin, static_cast<size_t>(pEnd - pBegin)}))
				{
					return false;
				}
			}
			if (!ValidTree(*it, iDepth + 1))
			{
				return false;
			}
		}
		return true;
	}

	static bool Matches(std::string_view sType)
	{
		sType = sType.substr(0, sType.find(';'));
		const auto iStart = sType.find_first_not_of(" \t");
		if (iStart == sType.npos)
		{
			return false;
		}
		sType = sType.substr(iStart, sType.find_last_not_of(" \t") - iStart + 1);
		if (HeaderFields::NameEqual(sType, "application/json"))
		{
			return true;
		}
		constexpr std::string_view sPrefix = "application/";
		constexpr std::string_view sSuffix = "+json";
		if (sType.size() <= sPrefix.size() + sSuffix.size() ||
		    !HeaderFields::NameEqual(sType.substr(0, sPrefix.size()), sPrefix) ||
		    !HeaderFields::NameEqual(sType.substr(sType.size() - sSuffix.size()), sSuffix))
		{
			return false;
		}
		auto sSubtype = sType.substr(sPrefix.size(), sType.size() - sPrefix.size() - sSuffix.size());
		return std::all_of(sSubtype.begin(), sSubtype.end(), [](unsigned char iByte)
		{
			return (iByte >= '0' && iByte <= '9') || (iByte >= 'a' && iByte <= 'z') ||
			       (iByte >= 'A' && iByte <= 'Z') || std::string_view("!#$&^_.+-").find(iByte) != std::string_view::npos;
		});
	}
};
}

int32_t Message::SetJson(const Json::Value& stValue)
{
	if (!JsonWire::ValidTree(stValue))
	{
		return OI_ERR_ENCODE;
	}
	Json::StreamWriterBuilder stBuilder;
	stBuilder["indentation"] = "";
	auto sWire = Json::writeString(stBuilder, stValue);
	if (sWire.size() > kJsonLimit)
	{
		return OI_ERR_ENCODE;
	}
	sBody = std::move(sWire);
	sContentType = "application/json";
	pBodySource.reset();
	return OI_OK;
}

int32_t Message::GetJson(Json::Value& stValue) const
{
	if (!JsonWire::Matches(sContentType))
	{
		return OI_ERR_PROTOCOL;
	}
	if (pBodySource || sBody.size() > kJsonLimit || sBody.find('\0') != sBody.npos || !JsonWire::ValidUtf8(sBody))
	{
		return OI_ERR_DECODE;
	}
	Json::CharReaderBuilder stBuilder;
	Json::CharReaderBuilder::strictMode(&stBuilder.settings_);
	stBuilder["strictRoot"] = false;
	stBuilder["stackLimit"] = 256;
	std::unique_ptr<Json::CharReader> pReader(stBuilder.newCharReader());
	Json::Value stJson;
	try
	{
		if (!pReader->parse(sBody.data(), sBody.data() + sBody.size(), &stJson, nullptr) || !JsonWire::ValidTree(stJson))
		{
			return OI_ERR_DECODE;
		}
	}
	catch (const Json::Exception&)
	{
		return OI_ERR_DECODE;
	}
	stValue.swap(stJson);
	return OI_OK;
}
}
