#pragma once
#include "oi_http_json.h"
#include "oi_rpcdata_jsonall.h"
#include <limits>
#include <map>
#include <string>
#include <vector>

class WebJsonRegression
{
  public:
	static bool Check()
	{
		using Value = std::map<std::string, std::vector<int64_t>>;
		const Value stInput{{std::string("a\0b", 3), {9007199254740993LL, -5}}};
		OI::Http::Response stMessage;
		if (stMessage.SetJson(stInput) != OI_OK)
		{
			return false;
		}
		Value stOutput;
		if (stMessage.GetJson(stOutput) != OI_OK || stOutput != stInput)
		{
			return false;
		}
		stMessage.sContentType = "application/json; charset=utf-8";
		if (stMessage.GetJson(stOutput) != OI_OK || stOutput != stInput)
		{
			return false;
		}
		stMessage.sContentType = "application/example+json";
		if (stMessage.GetJson(stOutput) != OI_OK || stOutput != stInput)
		{
			return false;
		}
		stMessage.sContentType = "text/plain";
		if (stMessage.GetJson(stOutput) != OI_ERR_PROTOCOL || stOutput != stInput)
		{
			return false;
		}
		OI::RpcData::JsonAll stData;
		stData.iGap = 17;
		stMessage.sContentType = "application/json";
		for (const auto& sBad : std::vector<std::string>{"{", "{} trailing", "{\"gap\":\"bad\"}"})
		{
			stMessage.sBody = sBad;
			if (stMessage.GetJson(stData) != OI_ERR_DECODE || stData.iGap != 17)
			{
				return false;
			}
		}
		stData.sMessage = std::string("a\0\xe9\x9b\xaa", 5);
		OI::Http::Response stUnicode;
		if (stUnicode.SetJson(stData) != OI_OK)
		{
			return false;
		}
		OI::RpcData::JsonAll stUnicodeBack;
		if (stUnicode.GetJson(stUnicodeBack) != OI_OK || stUnicodeBack.sMessage != stData.sMessage)
		{
			return false;
		}
		OI::Http::Response stKept;
		stKept.sBody = "previous body";
		stKept.sContentType = "text/plain";
		stKept.vHeaders.emplace_back("X-Keep", "previous");
		OI::RpcData::JsonAll stInvalidBytes;
		const char vRaw[] = {static_cast<char>(0xff), '\0'};
		stInvalidBytes.stRawData.Append(vRaw, sizeof(vRaw));
		if (stKept.SetJson(stInvalidBytes) != OI_ERR_ENCODE || stKept.sBody != "previous body" ||
		    stKept.sContentType != "text/plain" || stKept.vHeaders != OI::Http::Headers{{"X-Keep", "previous"}})
		{
			return false;
		}
		for (double dInvalid : {std::numeric_limits<double>::quiet_NaN(),
		         std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity()})
		{
			OI::RpcData::JsonAll stInvalidNumber;
			stInvalidNumber.dDouble = dInvalid;
			if (stKept.SetJson(stInvalidNumber) != OI_ERR_ENCODE || stKept.sBody != "previous body" ||
			    stKept.sContentType != "text/plain" || stKept.vHeaders != OI::Http::Headers{{"X-Keep", "previous"}})
			{
				return false;
			}
		}
		OI::Http::Response stInvalidWire;
		stInvalidWire.sContentType = "application/json";
		stInvalidWire.sBody = std::string("{\"msg\":\"", 8) + static_cast<char>(0xff) + "\"}";
		OI::RpcData::JsonAll stKeptOutput;
		stKeptOutput.sMessage = "previous";
		if (stInvalidWire.GetJson(stKeptOutput) != OI_ERR_DECODE || stKeptOutput.sMessage != "previous")
		{
			return false;
		}
		return true;
	}
};
