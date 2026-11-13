#include "web_client.h"
#include "oi_config.h"
#include "oi_http.h"
#include "oi_log.h"
#include "oi_rpc_coro.h"
#include "oi_rpcdata_jsonall.h"
#include "oi_thread.h"
#include "json/json.h"
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

static std::string DumpJson(const Json::Value& stValue)
{
	Json::StreamWriterBuilder stBuilder;
	stBuilder["indentation"] = "";
	return Json::writeString(stBuilder, stValue);
}

static bool ParseJson(const std::string& sText, Json::Value& stOutput)
{
	Json::CharReaderBuilder stBuilder;
	std::string sErrors;
	const std::unique_ptr<Json::CharReader> pReader(stBuilder.newCharReader());
	const char* pBegin = sText.data();
	return pReader->parse(pBegin, pBegin + sText.size(), &stOutput, &sErrors);
}

static int32_t OkStatus(const ::OI::Http::Response& stResponse)
{
	if (stResponse.iCode != OI_OK)
	{
		LOG_ERROR("web request failed code=%d status=%d", stResponse.iCode, stResponse.iStatus);
		return stResponse.iCode;
	}
	if (stResponse.iStatus != 200)
	{
		LOG_ERROR("web unexpected HTTP status=%d code=%d", stResponse.iStatus, OI_ERR_PROTOCOL);
		return OI_ERR_PROTOCOL;
	}
	return OI_OK;
}

// 别名键仍是 c、gap。
// [json] 不写名字，或者结构标了 json 而字段什么都不写，键都是字段名。
static bool PlainJsonKeys(const std::string& sText)
{
	Json::Value stWire;
	if (!ParseJson(sText, stWire) || !stWire.isObject())
	{
		return false;
	}
	if (!stWire.isMember("iPlain") || !stWire["iPlain"].isInt() || stWire["iPlain"].asInt() != 11)
	{
		return false;
	}
	if (!stWire.isMember("sPlain") || !stWire["sPlain"].isString() || stWire["sPlain"].asString() != "plain")
	{
		return false;
	}
	if (!stWire.isMember("vPlain") || !stWire["vPlain"].isArray() || stWire["vPlain"].size() != 1 || !stWire["vPlain"][0].isInt() || stWire["vPlain"][0].asInt() != 5)
	{
		return false;
	}
	if (!stWire.isMember("mPlain") || !stWire["mPlain"].isObject() || !stWire["mPlain"].isMember("k") || !stWire["mPlain"]["k"].isInt() || stWire["mPlain"]["k"].asInt() != 6)
	{
		return false;
	}
	// clang-format off
	if (!stWire.isMember("stPlain")
		|| !stWire["stPlain"].isObject()
		|| !stWire["stPlain"].isMember("iNumber")
		|| stWire["stPlain"]["iNumber"].asInt() != 8
		|| !stWire["stPlain"].isMember("sText")
		|| stWire["stPlain"]["sText"].asString() != "same")
	{
		return false;
	}
	// clang-format on
	if (!stWire.isMember("iDefault") || !stWire["iDefault"].isInt() || stWire["iDefault"].asInt() != 12)
	{
		return false;
	}
	if (!stWire.isMember("sDefault") || !stWire["sDefault"].isString() || stWire["sDefault"].asString() != "def")
	{
		return false;
	}
	if (!stWire.isMember("vDefault") || !stWire["vDefault"].isArray() || stWire["vDefault"].size() != 1 || !stWire["vDefault"][0].isInt() || stWire["vDefault"][0].asInt() != 7)
	{
		return false;
	}
	if (!stWire.isMember("mDefault") || !stWire["mDefault"].isObject() || !stWire["mDefault"].isMember("d") || !stWire["mDefault"]["d"].isInt() || stWire["mDefault"]["d"].asInt() != 8)
	{
		return false;
	}
	// clang-format off
	if (!stWire.isMember("stDefault")
		|| !stWire["stDefault"].isObject()
		|| !stWire["stDefault"].isMember("iNumber")
		|| stWire["stDefault"]["iNumber"].asInt() != 9
		|| !stWire["stDefault"].isMember("sText")
		|| stWire["stDefault"]["sText"].asString() != "bare")
	{
		return false;
	}
	// clang-format on
	if (stWire["stDefault"].isMember("iSkip") || stWire.isMember("iSkip") || stWire.isMember("plain"))
	{
		return false;
	}
	if (!stWire.isMember("c") || !stWire.isMember("gap"))
	{
		return false;
	}
	return true;
}

static OI::Coro::Task Run()
{
	std::string sIpLocal = "127.0.0.1";
	int32_t iPortLocal = 18080;
	TryGetConfig("WebClient.ip", sIpLocal);
	TryGetConfig("WebClient.port", iPortLocal);

	::OI::Http::Client stCli(sIpLocal, iPortLocal);
	int iBadLocal = 0;

	const std::string sText = "a b+";
	std::vector<std::pair<std::string, std::string>> vQuery;
	vQuery.push_back(std::make_pair(std::string("text"), sText));
	::OI::Http::Response stGot = co_await stCli.Get("/query", vQuery);
	std::string sGotTextGottext;
	if (OkStatus(stGot) != OI_OK)
	{
		iBadLocal += 1;
	}
	else
	{
		Json::Value stBody;
		if (!ParseJson(stGot.sBody, stBody) || !stBody.isMember("text") || !stBody["text"].isString())
		{
			iBadLocal += 1;
		}
		else
		{
			sGotTextGottext = stBody["text"].asString();
			if (sGotTextGottext != sText)
			{
				iBadLocal += 1;
			}
		}
	}

	Json::Value stRequestLocal(Json::objectValue);
	stRequestLocal["n"] = 1;
	stRequestLocal["s"] = "hi";
	::OI::Http::Response stJs = co_await stCli.Post("/json", DumpJson(stRequestLocal), "application/json");
	int iGotNumber = 0;
	bool bGotOk = false;
	std::string sGotTextGots;
	if (OkStatus(stJs) != OI_OK)
	{
		iBadLocal += 1;
	}
	else
	{
		Json::Value stBody;
		if (!ParseJson(stJs.sBody, stBody) || !stBody.isMember("n") || !stBody.isMember("s") || !stBody.isMember("ok"))
		{
			iBadLocal += 1;
		}
		else
		{
			iGotNumber = stBody["n"].asInt();
			sGotTextGots = stBody["s"].asString();
			bGotOk = stBody["ok"].asBool();
			if (iGotNumber != 1 || sGotTextGots != "hi" || !bGotOk)
			{
				iBadLocal += 1;
			}
		}
	}

	::OI::RpcData::JsonAll stSent;
	stSent.iInt8 = 7;
	stSent.iInt16 = 1001;
	stSent.iInt32 = 42;
	stSent.iInt64 = 100000;
	stSent.iAlternateInt64 = 10000000000LL;
	stSent.fFloat = 1.5f;
	stSent.dDouble = 2.5;
	stSent.stRawData.Append("raw", 3);
	stSent.sMessage = "msg";
	stSent.vNumbers.push_back(1);
	stSent.vNumbers.push_back(2);
	stSent.vWords.push_back("w");
	stSent.vWords.push_back("ord");
	stSent.stChild.iNumber = 3;
	stSent.stChild.sText = "child";
	::OI::RpcData::Inner stKin;
	stKin.iNumber = 9;
	stKin.sText = "kin";
	stSent.vChildren.push_back(stSent.stChild);
	stSent.vChildren.push_back(stKin);
	stSent.mTable[1] = "t";
	stSent.mTable[2] = "u";
	stSent.mNamed["a"] = stSent.stChild;
	stSent.mNamed["b"] = stKin;
	stSent.iUnsigned8 = 200;
	stSent.iUnsigned16 = 40000;
	stSent.iUnsigned32 = 3000000000u;
	stSent.iUnsigned64 = (1ULL << 63) + 9;
	stSent.vUnsignedNumbers.push_back(stSent.iUnsigned64);
	stSent.vUnsignedNumbers.push_back(1);
	stSent.iGap = 20;
	stSent.mUnsignedTable[stSent.iUnsigned32] = "u";
	stSent.mLargeUnsignedTable[stSent.iUnsigned64] = "b";
	stSent.iPlain = 11;
	stSent.sPlain = "plain";
	stSent.vPlain.push_back(5);
	stSent.mPlain["k"] = 6;
	stSent.stPlain.iNumber = 8;
	stSent.stPlain.sText = "same";
	stSent.iDefault = 12;
	stSent.sDefault = "def";
	stSent.vDefault.push_back(7);
	stSent.mDefault["d"] = 8;
	stSent.stDefault.iNumber = 9;
	stSent.stDefault.sText = "bare";
	stSent.stDefault.iSkip = 77;
	stSent.iSkip = 99;
	::OI::RpcData::JsonAll stKept;
	stKept.iSkip = 99;
	stKept.stDefault.iSkip = 77;
	if (!stKept.FromJson(std::string("{\"iSkip\":1,\"stDefault\":{\"iNumber\":9,\"sText\":\"bare\",\"iSkip\":2}}")) || stKept.iSkip != 99 || stKept.stDefault.iSkip != 77 || stKept.stDefault.iNumber != 9)
	{
		iBadLocal += 1;
	}
	const std::string sRpcJson = stSent.ToJson();
	::OI::Http::Response stRpc = co_await stCli.Post("/rpcjson", sRpcJson, "application/json");
	int iRpcOk = 0;
	if (OkStatus(stRpc) != OI_OK)
	{
		iBadLocal += 1;
	}
	else
	{
		::OI::RpcData::JsonAll stBack;
		// clang-format off
		bool bSame = PlainJsonKeys(sRpcJson)
			&& PlainJsonKeys(stRpc.sBody)
			&& stBack.FromJson(stRpc.sBody)
			&& stBack.iInt8 == stSent.iInt8
			&& stBack.iInt16 == stSent.iInt16
			&& stBack.iInt32 == stSent.iInt32
			&& stBack.iInt64 == stSent.iInt64
			&& stBack.iAlternateInt64 == stSent.iAlternateInt64
			&& stBack.fFloat == stSent.fFloat
			&& stBack.dDouble == stSent.dDouble
			&& stBack.stRawData.Size() == stSent.stRawData.Size()
			&& (stSent.stRawData.Size() == 0 || std::memcmp(stBack.stRawData.Data(), stSent.stRawData.Data(), stSent.stRawData.Size()) == 0)
			&& stBack.sMessage == stSent.sMessage
			&& stBack.vNumbers == stSent.vNumbers
			&& stBack.vWords == stSent.vWords
			&& stBack.stChild.iNumber == stSent.stChild.iNumber
			&& stBack.stChild.sText == stSent.stChild.sText
			&& stBack.vChildren.size() == stSent.vChildren.size()
			&& stBack.mTable == stSent.mTable
			&& stBack.mNamed.size() == stSent.mNamed.size()
			&& stBack.iUnsigned8 == stSent.iUnsigned8
			&& stBack.iUnsigned16 == stSent.iUnsigned16
			&& stBack.iUnsigned32 == stSent.iUnsigned32
			&& stBack.iUnsigned64 == stSent.iUnsigned64
			&& stBack.vUnsignedNumbers == stSent.vUnsignedNumbers
			&& stBack.iGap == stSent.iGap + 1
			&& stBack.mUnsignedTable == stSent.mUnsignedTable
			&& stBack.mLargeUnsignedTable == stSent.mLargeUnsignedTable
			&& stBack.iPlain == stSent.iPlain
			&& stBack.sPlain == stSent.sPlain
			&& stBack.vPlain == stSent.vPlain
			&& stBack.mPlain == stSent.mPlain
			&& stBack.stPlain.iNumber == stSent.stPlain.iNumber
			&& stBack.stPlain.sText == stSent.stPlain.sText
			&& stBack.iDefault == stSent.iDefault
			&& stBack.sDefault == stSent.sDefault
			&& stBack.vDefault == stSent.vDefault
			&& stBack.mDefault == stSent.mDefault
			&& stBack.stDefault.iNumber == stSent.stDefault.iNumber
			&& stBack.stDefault.sText == stSent.stDefault.sText
			&& stBack.iSkip == 0
			&& stBack.stDefault.iSkip == 0;
		// clang-format on
		if (bSame)
		{
			for (size_t iIndexLocal = 0; iIndexLocal < stSent.vChildren.size(); ++iIndexLocal)
			{
				if (stBack.vChildren[iIndexLocal].iNumber != stSent.vChildren[iIndexLocal].iNumber || stBack.vChildren[iIndexLocal].sText != stSent.vChildren[iIndexLocal].sText)
				{
					bSame = false;
					break;
				}
			}
		}
		if (bSame)
		{
			for (const auto& [sName, stValue] : stSent.mNamed)
			{
				auto stJt = stBack.mNamed.find(sName);
				if (stJt == stBack.mNamed.end() || stJt->second.iNumber != stValue.iNumber || stJt->second.sText != stValue.sText)
				{
					bSame = false;
					break;
				}
			}
		}
		if (bSame)
		{
			iRpcOk = 1;
		}
		else
		{
			iBadLocal += 1;
		}
	}

	std::string sBin("\x00\x01\xff", 3);
	::OI::Http::Response stRaw = co_await stCli.Post("/bin", sBin, "application/octet-stream");
	int iBinOk = 0;
	if (OkStatus(stRaw) == OI_OK && stRaw.sBody.size() == sBin.size() && std::memcmp(stRaw.sBody.data(), sBin.data(), sBin.size()) == 0)
	{
		iBinOk = 1;
	}
	else
	{
		iBadLocal += 1;
	}

	LOG_TRACE("web query=%d text=%s json_n=%d json_s=%s json_ok=%d rpc=%d bin=%d bad=%d",
	    sGotTextGottext == sText ? 1 : 0, sGotTextGottext.c_str(), iGotNumber, sGotTextGots.c_str(), bGotOk ? 1 : 0, iRpcOk, iBinOk, iBadLocal);
	if (iBadLocal != 0)
	{
		LOG_TRACE("web err query_code=%d json_code=%d rpc_code=%d bin_code=%d",
		    stGot.iCode, stJs.iCode, stRpc.iCode, stRaw.iCode);
	}
	fflush(stdout);
	::OI::Thread::ThreadPool::GetInstance().RequestStop(iBadLocal == 0 ? 0 : 1);
}

void WebClient::Start()
{
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return Run();
	});
}
