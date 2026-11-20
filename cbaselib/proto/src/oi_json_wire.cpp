#include "oi_json_wire.h"
#include <cmath>

namespace OI
{
namespace Rpc
{
namespace
{
constexpr size_t kJsonDepthLimit = 256;

bool ValidTree(const Json::Value& stValue, size_t iDepth)
{
	if (iDepth > kJsonDepthLimit)
	{
		return false;
	}
	if (stValue.isString())
	{
		const char* pBegin = nullptr;
		const char* pEnd = nullptr;
		return stValue.getString(&pBegin, &pEnd) && ValidJsonUtf8({pBegin, static_cast<size_t>(pEnd - pBegin)});
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
			if (!ValidJsonUtf8({pBegin, static_cast<size_t>(pEnd - pBegin)}))
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
}

bool ValidJsonUtf8(std::string_view sText)
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
		size_t iCount = 1;
		if (iLead >= 0xf0)
		{
			iCount = 3;
		}
		else if (iLead >= 0xe0)
		{
			iCount = 2;
		}
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

bool ValidJsonTree(const Json::Value& stValue)
{
	return ValidTree(stValue, 1);
}
}
}
