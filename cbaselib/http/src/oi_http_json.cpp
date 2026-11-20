#include "oi_http_header_fields.h"
#include "oi_json_wire.h"

namespace OI::Http
{
namespace
{
constexpr size_t kJsonLimit = 8 * 1024 * 1024;

bool MatchJsonType(std::string_view sType)
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
}

int32_t Message::SetJson(const Json::Value& stValue)
{
	if (!Rpc::ValidJsonTree(stValue))
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
	if (!MatchJsonType(sContentType))
	{
		return OI_ERR_PROTOCOL;
	}
	if (pBodySource || sBody.size() > kJsonLimit || sBody.find('\0') != sBody.npos || !Rpc::ValidJsonUtf8(sBody))
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
		if (!pReader->parse(sBody.data(), sBody.data() + sBody.size(), &stJson, nullptr) || !Rpc::ValidJsonTree(stJson))
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
