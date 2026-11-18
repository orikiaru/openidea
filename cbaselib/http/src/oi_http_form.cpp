#include "oi_http_header_fields.h"
#include <atomic>
#include <charconv>

namespace OI::Http
{
namespace
{
using View = std::string_view;
class FormCodec
{
  public:
	static constexpr size_t Limit = 8 * 1024 * 1024;
	static constexpr size_t Parts = 1024;

	static View Trim(View sValue)
	{
		auto iStart = sValue.find_first_not_of(" \t");
		return iStart == View::npos ? View{} : sValue.substr(iStart, sValue.find_last_not_of(" \t") - iStart + 1);
	}

	static bool Safe(View sValue)
	{
		return std::all_of(sValue.begin(), sValue.end(), [](unsigned char iByte)
		{
			return iByte >= 32 && iByte != 127;
		});
	}

	static bool Append(std::string& sOut, View sData)
	{
		if (sData.size() > Limit - sOut.size())
		{
			return false;
		}
		sOut.append(sData);
		return true;
	}

	static bool Encode(std::string& sOut, View sValue)
	{
		constexpr char vDigits[] = "0123456789ABCDEF";
		for (unsigned char iByte : sValue)
		{
			char vBytes[3] = {'%', vDigits[iByte >> 4], vDigits[iByte & 15]};
			size_t iSize = 3;
			if ((iByte >= 'a' && iByte <= 'z') || (iByte >= 'A' && iByte <= 'Z') || (iByte >= '0' && iByte <= '9') ||
			    View("-._* ").find(static_cast<char>(iByte)) != View::npos)
			{
				vBytes[0] = iByte == ' ' ? '+' : static_cast<char>(iByte);
				iSize = 1;
			}
			if (!Append(sOut, View(vBytes, iSize)))
			{
				return false;
			}
		}
		return true;
	}

	static bool Decode(View sValue, std::string& sOut)
	{
		for (size_t i = 0; i < sValue.size(); ++i)
		{
			if (sValue[i] != '%')
			{
				sOut.push_back(sValue[i] == '+' ? ' ' : sValue[i]);
				continue;
			}
			if (sValue.size() - i < 3)
			{
				return false;
			}
			const char* pEnd = sValue.data() + i + 3;
			unsigned iByte = 0;
			auto stRet = std::from_chars(pEnd - 2, pEnd, iByte, 16);
			if (stRet.ec != std::errc{} || stRet.ptr != pEnd)
			{
				return false;
			}
			sOut.push_back(static_cast<char>(iByte));
			i += 2;
		}
		return true;
	}

	static bool Quoted(std::string& sOut, View sValue)
	{
		if (!Safe(sValue) || !Append(sOut, "\""))
		{
			return false;
		}
		for (char iChar : sValue)
		{
			if ((iChar == '\\' || iChar == '"') && !Append(sOut, "\\"))
			{
				return false;
			}
			if (!Append(sOut, View(&iChar, 1)))
			{
				return false;
			}
		}
		return Append(sOut, "\"");
	}

	// Consume semicolon parameters, including quoted strings containing semicolons.
	static bool Params(View sText, View sType, Headers& vOutput)
	{
		size_t iEnd = sText.find(';');
		if (!HeaderFields::NameEqual(Trim(sText.substr(0, iEnd)), sType))
		{
			return false;
		}
		while (iEnd != View::npos)
		{
			sText = Trim(sText.substr(iEnd + 1));
			auto iEq = sText.find_first_of("=;");
			if (iEq == View::npos || sText[iEq] != '=' || Trim(sText.substr(0, iEq)).empty())
			{
				return false;
			}
			auto sKey = Trim(sText.substr(0, iEq));
			sText = Trim(sText.substr(iEq + 1));
			std::string sValue;
			if (sText.starts_with('"'))
			{
				size_t i = 1;
				for (; i < sText.size() && sText[i] != '"'; ++i)
				{
					if (sText[i] == '\\' && ++i == sText.size())
					{
						return false;
					}
					sValue.push_back(sText[i]);
				}
				if (i == sText.size())
				{
					return false;
				}
				sText = Trim(sText.substr(i + 1));
				if (!sText.empty() && sText.front() != ';')
				{
					return false;
				}
				iEnd = sText.empty() ? View::npos : 0;
			}
			else
			{
				iEnd = sText.find(';');
				sValue = Trim(sText.substr(0, iEnd));
			}
			if (!Safe(sValue) || vOutput.size() >= 64)
			{
				return false;
			}
			vOutput.emplace_back(sKey, std::move(sValue));
		}
		return true;
	}

	static bool Boundary(View sBoundary)
	{
		return !sBoundary.empty() && sBoundary.size() <= 70 && sBoundary.back() != ' ' &&
		       std::all_of(sBoundary.begin(), sBoundary.end(), [](unsigned char iChar)
		{
			return (iChar >= 'A' && iChar <= 'Z') || (iChar >= 'a' && iChar <= 'z') || (iChar >= '0' && iChar <= '9') ||
			       View("'()+_,-./:=? ").find(static_cast<char>(iChar)) != View::npos;
		});
	}

	static bool PartHeaders(View sHeaders, FormPart& stPart)
	{
		bool bName = false, bDisposition = false, bType = false;
		while (!sHeaders.empty())
		{
			auto iEnd = sHeaders.find("\r\n");
			const auto sLine = sHeaders.substr(0, iEnd);
			auto iColon = sLine.find(':');
			if (iColon == View::npos || !Safe(sLine))
			{
				return false;
			}
			auto sKey = sLine.substr(0, iColon);
			auto sValue = Trim(sLine.substr(iColon + 1));
			if (HeaderFields::NameEqual(sKey, "content-disposition"))
			{
				Headers vParams;
				if (bDisposition || !Params(sValue, "form-data", vParams))
				{
					return false;
				}
				bDisposition = true;
				for (auto& [sParam, sData] : vParams)
				{
					if (HeaderFields::NameEqual(sParam, "name"))
					{
						if (bName)
						{
							return false;
						}
						bName = true;
						stPart.sName = std::move(sData);
					}
					else if (HeaderFields::NameEqual(sParam, "filename"))
					{
						if (stPart.sFilename)
						{
							return false;
						}
						stPart.sFilename = std::move(sData);
					}
				}
			}
			else if (HeaderFields::NameEqual(sKey, "content-type"))
			{
				if (bType)
				{
					return false;
				}
				bType = true;
				stPart.sContentType = sValue;
			}
			sHeaders = iEnd == View::npos ? View{} : sHeaders.substr(iEnd + 2);
		}
		return bName;
	}
};
}

int32_t Message::SetForm(const QueryParams& stForm)
{
	std::string sWire;
	size_t iCount = 0;
	for (const auto& [sKey, sValue] : stForm)
	{
		if (++iCount > FormCodec::Parts || (iCount > 1 && !FormCodec::Append(sWire, "&")) ||
		    !FormCodec::Encode(sWire, sKey) || !FormCodec::Append(sWire, "=") || !FormCodec::Encode(sWire, sValue))
		{
			return OI_ERR_INVALID_ARGUMENT;
		}
	}
	sContentType = "application/x-www-form-urlencoded";
	sBody = std::move(sWire);
	pBodySource.reset();
	return OI_OK;
}

int32_t Message::GetForm(QueryParams& stForm) const
{
	View sInput = sBody;
	if (!HeaderFields::NameEqual(FormCodec::Trim(View(sContentType).substr(0, sContentType.find(';'))),
	        "application/x-www-form-urlencoded") ||
	    sInput.size() > FormCodec::Limit || pBodySource)
	{
		return OI_ERR_PROTOCOL;
	}
	QueryParams stResult;
	size_t iCount = 0;
	while (!sInput.empty())
	{
		auto iEnd = sInput.find('&');
		auto sPair = sInput.substr(0, iEnd);
		if (!sPair.empty())
		{
			auto iEq = sPair.find('=');
			std::string sKey, sValue;
			if (++iCount > FormCodec::Parts || !FormCodec::Decode(sPair.substr(0, iEq), sKey) ||
			    (iEq != View::npos && !FormCodec::Decode(sPair.substr(iEq + 1), sValue)))
			{
				return OI_ERR_PROTOCOL;
			}
			stResult.Add(std::move(sKey), std::move(sValue));
		}
		sInput = iEnd == View::npos ? View{} : sInput.substr(iEnd + 1);
	}
	stForm = std::move(stResult);
	return OI_OK;
}

int32_t Message::SetMultipart(const std::vector<FormPart>& vParts)
{
	if (vParts.size() > FormCodec::Parts)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	static std::atomic<uint64_t> iSequence{0};
	std::string sBoundary;
	// At most one body scan for ordinary input. Bound collision retries.
	for (int iAttempt = 0; iAttempt < 16; ++iAttempt)
	{
		sBoundary = "openidea-" + std::to_string(++iSequence);
		bool bCollision = false;
		for (const auto& stPart : vParts)
		{
			if (stPart.sValue.size() > FormCodec::Limit)
			{
				return OI_ERR_INVALID_ARGUMENT;
			}
			bCollision |= stPart.sValue.find(sBoundary) != std::string::npos;
		}
		if (!bCollision)
		{
			break;
		}
		sBoundary.clear();
	}
	if (sBoundary.empty())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	std::string sWire;
	for (const auto& stPart : vParts)
	{
		const size_t iHeadersAt = sWire.size() + sBoundary.size() + 4;
		if (!FormCodec::Append(sWire, "--" + sBoundary + "\r\nContent-Disposition: form-data; name=") ||
		    !FormCodec::Quoted(sWire, stPart.sName) ||
		    (stPart.sFilename && (!FormCodec::Append(sWire, "; filename=") || !FormCodec::Quoted(sWire, *stPart.sFilename))) ||
		    (!stPart.sContentType.empty() && (!FormCodec::Safe(stPart.sContentType) ||
		                                         !FormCodec::Append(sWire, "\r\nContent-Type: ") || !FormCodec::Append(sWire, stPart.sContentType))) ||
		    sWire.size() - iHeadersAt > 65536 || !FormCodec::Append(sWire, "\r\n\r\n") ||
		    !FormCodec::Append(sWire, stPart.sValue) || !FormCodec::Append(sWire, "\r\n"))
		{
			return OI_ERR_INVALID_ARGUMENT;
		}
	}
	if (!FormCodec::Append(sWire, "--" + sBoundary + "--\r\n"))
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	sContentType = "multipart/form-data; boundary=" + sBoundary;
	sBody = std::move(sWire);
	pBodySource.reset();
	return OI_OK;
}

int32_t Message::GetMultipart(std::vector<FormPart>& vParts) const
{
	Headers vParams;
	if (pBodySource || sBody.size() > FormCodec::Limit || sContentType.size() > 65536 ||
	    !FormCodec::Params(sContentType, "multipart/form-data", vParams))
	{
		return OI_ERR_PROTOCOL;
	}
	std::string sMarker;
	for (const auto& [sKey, sValue] : vParams)
	{
		if (HeaderFields::NameEqual(sKey, "boundary"))
		{
			if (!sMarker.empty() || !FormCodec::Boundary(sValue))
			{
				return OI_ERR_PROTOCOL;
			}
			sMarker = "--" + sValue;
		}
	}
	View sInput = sBody;
	if (sMarker.empty() || !sInput.starts_with(sMarker))
	{
		return OI_ERR_PROTOCOL;
	}
	sInput.remove_prefix(sMarker.size());
	const std::string sNext = "\r\n" + sMarker;
	std::vector<FormPart> vResult;
	while (!sInput.starts_with("--"))
	{
		if (!sInput.starts_with("\r\n") || vResult.size() >= FormCodec::Parts)
		{
			return OI_ERR_PROTOCOL;
		}
		sInput.remove_prefix(2);
		auto iEnd = sInput.find("\r\n\r\n");
		auto& stPart = vResult.emplace_back();
		if (iEnd == View::npos || iEnd > 65536 || !FormCodec::PartHeaders(sInput.substr(0, iEnd), stPart))
		{
			return OI_ERR_PROTOCOL;
		}
		sInput.remove_prefix(iEnd + 4);
		size_t iAt = 0;
		while ((iAt = sInput.find(sNext, iAt)) != View::npos)
		{
			View sTail = sInput.substr(iAt + sNext.size());
			if (sTail.starts_with("\r\n") || sTail == "--" || sTail.starts_with("--\r\n"))
			{
				break;
			}
			++iAt;
		}
		if (iAt == View::npos)
		{
			return OI_ERR_PROTOCOL;
		}
		stPart.sValue = sInput.substr(0, iAt);
		sInput.remove_prefix(iAt + sNext.size());
	}
	if (sInput != "--" && !sInput.starts_with("--\r\n"))
	{
		return OI_ERR_PROTOCOL;
	}
	vParts = std::move(vResult);
	return OI_OK;
}
}
