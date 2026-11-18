#pragma once
#include "oi_http_message.h"
#include "oi_json.h"

namespace OI::Http
{
template <typename T>
int32_t Message::SetJson(const T& stValue)
{
	Json::Value stJson;
	Rpc::ToJsonValue(stJson, stValue);
	return SetJson(stJson);
}

template <typename T>
int32_t Message::GetJson(T& stValue) const
{
	Json::Value stJson;
	const int32_t iCode = GetJson(stJson);
	if (iCode != OI_OK)
	{
		return iCode;
	}
	T stDecoded(stValue);
	if (!Rpc::FromJsonValue(stJson, stDecoded))
	{
		return OI_ERR_DECODE;
	}
	stValue = std::move(stDecoded);
	return OI_OK;
}
}
