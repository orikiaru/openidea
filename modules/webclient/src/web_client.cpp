#include "web_client.h"
#include <optional>
#include "oi_http_json.h"
#include "web_stream_fixture.h"
#include "web_json_regression.h"
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
		LOG_ERROR("web request failed code=%d status=%d reason=%s", stResponse.iCode, stResponse.iStatus, stResponse.sError.c_str());
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

OI::Coro::Task WebClient::PoolGet(std::string sUrl, int32_t iTimeoutMilliseconds, bool bFresh, OI::Http::Response& stResponse)
{
	OI::Http::ClientOptions stOptions;
	stOptions.iTimeoutMilliseconds = iTimeoutMilliseconds;
	OI::Http::Request stRequest;
	stRequest.sMethod = "GET";
	stRequest.bFreshConnection = bFresh;
	stResponse = co_await OI::Http::Client::SendUrl(std::move(sUrl), stRequest, stOptions);
}

OI::Coro::Task WebClient::PoolRelease(std::string sControlUrl, OI::Http::Response& stResponse)
{
	co_await OI::Coro::yield(700);
	stResponse = co_await OI::Http::Client::GetUrl(sControlUrl + "/release");
}

OI::Coro::Task WebClient::PoolQueue(std::string sBase, std::string sControlUrl, OI::Http::Response& stQueued, OI::Http::Response& stQueuedSuccess, OI::Http::Response& stReady, OI::Http::Response& stRelease)
{
	stReady = co_await OI::Http::Client::GetUrl(sControlUrl + "/ready");
	if (stReady.iCode != OI_OK)
	{
		co_return;
	}
	std::vector<OI::Coro::Task> vTasks;
	vTasks.push_back(PoolGet(sBase + "/queued", 300, false, stQueued));
	vTasks.push_back(PoolGet(sBase + "/queued-success", 3000, false, stQueuedSuccess));
	vTasks.push_back(PoolRelease(std::move(sControlUrl), stRelease));
	co_await OI::Coro::WhenAll(std::move(vTasks));
}

OI::Coro::Task WebClient::RunPoolRegression(int32_t iPort, int32_t iControlPort)
{
	const std::string sBase = "http://127.0.0.1:" + std::to_string(iPort);
	const std::string sControlUrl = "http://127.0.0.1:" + std::to_string(iControlPort);
	OI::Http::Response stHoldOne;
	OI::Http::Response stHoldTwo;
	OI::Http::Response stQueued;
	OI::Http::Response stQueuedSuccess;
	OI::Http::Response stReady;
	OI::Http::Response stRelease;
	std::vector<OI::Coro::Task> vTasks;
	vTasks.push_back(PoolGet(sBase + "/hold-one", 5000, false, stHoldOne));
	vTasks.push_back(PoolGet(sBase + "/hold-two", 5000, false, stHoldTwo));
	vTasks.push_back(PoolQueue(sBase, sControlUrl, stQueued, stQueuedSuccess, stReady, stRelease));
	co_await OI::Coro::WhenAll(std::move(vTasks));
	auto stAfterQueue = co_await OI::Http::Client::GetUrl(sBase + "/after-queue");
	OI::Http::Response stBusy;
	OI::Http::Response stFresh;
	std::vector<OI::Coro::Task> vFreshTasks;
	vFreshTasks.push_back(PoolGet(sBase + "/busy", 3000, false, stBusy));
	vFreshTasks.push_back(PoolGet(sBase + "/fresh-full", 3000, true, stFresh));
	co_await OI::Coro::WhenAll(std::move(vFreshTasks));
	co_await OI::Coro::yield(5500);
	auto stAfterIdle = co_await OI::Http::Client::GetUrl(sBase + "/after-idle");
	int32_t iBad = 0;
	if (stHoldOne.iCode != OI_OK || stHoldOne.sBody != "/hold-one" || stHoldTwo.iCode != OI_OK || stHoldTwo.sBody != "/hold-two" || stQueued.iCode != OI_ERR_TIMEOUT || stQueuedSuccess.iCode != OI_OK || stQueuedSuccess.sBody != "/queued-success" || stReady.iCode != OI_OK || stRelease.iCode != OI_OK || stAfterQueue.iCode != OI_OK || stAfterQueue.sBody != "/after-queue" || stBusy.iCode != OI_OK || stBusy.sBody != "/busy" || stFresh.iCode != OI_OK || stFresh.sBody != "/fresh-full" || stAfterIdle.iCode != OI_OK || stAfterIdle.sBody != "/after-idle")
	{
		LOG_ERROR("pool regression failed hold1=%d hold2=%d queued=%d queued_success=%d ready=%d release=%d after=%d busy=%d fresh=%d idle=%d", stHoldOne.iCode, stHoldTwo.iCode, stQueued.iCode, stQueuedSuccess.iCode, stReady.iCode, stRelease.iCode, stAfterQueue.iCode, stBusy.iCode, stFresh.iCode, stAfterIdle.iCode);
		++iBad;
	}
	LOG_TRACE("pool regression bad=%d", iBad);
	fflush(stdout);
	OI::Thread::ThreadPool::GetInstance().RequestStop(iBad == 0 ? 0 : 1);
}

OI::Coro::Task WebClient::UrlGet(std::string sUrl, OI::Http::Response& stResponse)
{
	stResponse = co_await OI::Http::Client::GetUrl(std::move(sUrl));
}

OI::Coro::Task WebClient::RunUrlRegression(int32_t iPort, int32_t iTlsPort, const std::string& sCaFile)
{
	const std::string sBase = "http://127.0.0.1:" + std::to_string(iPort);
	const std::string sTlsBase = "https://localhost:" + std::to_string(iTlsPort);
	int32_t iBad = 0;
	auto stFirst = co_await OI::Http::Client("127.0.0.1", iPort).Get("/first");
	auto stSecond = co_await OI::Http::Client::GetUrl(sBase + "/second?x=1");
	auto stThird = co_await OI::Http::Client("127.0.0.1", iPort).Get("/third");
	OI::Http::Request stFreshRequest;
	stFreshRequest.sMethod = "GET";
	stFreshRequest.bFreshConnection = true;
	auto stFresh = co_await OI::Http::Client::SendUrl(sBase + "/fresh", stFreshRequest);
	auto stAfter = co_await OI::Http::Client::GetUrl(sBase + "/after");
	OI::Http::Response stSlow;
	OI::Http::Response stFast;
	std::vector<OI::Coro::Task> vTasks;
	vTasks.push_back(UrlGet(sBase + "/slow", stSlow));
	vTasks.push_back(UrlGet(sBase + "/fast", stFast));
	co_await OI::Coro::WhenAll(std::move(vTasks));
	auto stTemporaryCall = OI::Http::Client::GetUrl(std::string(sBase + "/temporary"));
	auto stTemporary = co_await std::move(stTemporaryCall);
	auto stPost = co_await OI::Http::Client::PostUrl(sBase + "/post", "payload", "text/plain");
	OI::Http::ClientOptions stOptions;
	stOptions.sCaFile = sCaFile;
	stOptions.iTimeoutMilliseconds = 1500;
	auto stTls = co_await OI::Http::Client::GetUrl(sTlsBase + "/tls", stOptions);
	OI::Http::ClientOptions stUntrusted;
	stUntrusted.iTimeoutMilliseconds = 1500;
	auto stTlsUntrusted = co_await OI::Http::Client::GetUrl(sTlsBase + "/untrusted", stUntrusted);
	auto stWrongScheme = co_await OI::Http::Client::GetUrl("http://localhost:" + std::to_string(iTlsPort) + "/wrong", stOptions);
	auto stInvalid = co_await OI::Http::Client::GetUrl("http://:80/bad");
	if (stFirst.iCode != OI_OK || stFirst.sBody != "/first" || stSecond.iCode != OI_OK || stSecond.sBody != "/second?x=1" || stThird.iCode != OI_OK || stThird.sBody != "/third" || stFresh.iCode != OI_OK || stFresh.sBody != "/fresh" || stAfter.iCode != OI_OK || stAfter.sBody != "/after" || stSlow.iCode != OI_OK || stSlow.sBody != "/slow" || stFast.iCode != OI_OK || stFast.sBody != "/fast" || stTemporary.iCode != OI_OK || stTemporary.sBody != "/temporary" || stPost.iCode != OI_OK || stPost.sBody != "payload" || stTls.iCode != OI_OK || stTls.sBody != "/tls" || stTlsUntrusted.iCode != OI_ERR_CONNECT || stWrongScheme.iCode == OI_OK || stInvalid.iCode == OI_OK)
	{
		LOG_ERROR("url regression failed first=%d second=%d third=%d fresh=%d after=%d slow=%d fast=%d temporary=%d post=%d tls=%d untrusted=%d scheme=%d invalid=%d", stFirst.iCode, stSecond.iCode, stThird.iCode, stFresh.iCode, stAfter.iCode, stSlow.iCode, stFast.iCode, stTemporary.iCode, stPost.iCode, stTls.iCode, stTlsUntrusted.iCode, stWrongScheme.iCode, stInvalid.iCode);
		++iBad;
	}
	LOG_TRACE("url regression bad=%d", iBad);
	fflush(stdout);
	OI::Thread::ThreadPool::GetInstance().RequestStop(iBad == 0 ? 0 : 1);
}

OI::Coro::Task WebClient::TlsGet(OI::Http::Client& stClient, OI::Http::Response& stResponse)
{
	stResponse = co_await stClient.Get("/health");
}

OI::Coro::Task WebClient::RunTlsRegression(const std::string& sHost, int32_t iPort, const std::string& sCaFile, const std::string& sCase, int32_t iStallPort)
{
	OI::Http::Client stClient(sHost, iPort, OI::Http::TlsOptions{sCaFile}, 1500, true);
	int32_t iBad = 0;
	if (sCase == "stall")
	{
		OI::Http::Client stStall(sHost, iStallPort, OI::Http::TlsOptions{sCaFile}, 1500, true);
		OI::Http::Response stStalled;
		OI::Http::Response stHealthy;
		std::vector<OI::Coro::Task> vTasks;
		vTasks.push_back(TlsGet(stStall, stStalled));
		vTasks.push_back(TlsGet(stClient, stHealthy));
		co_await OI::Coro::WhenAll(std::move(vTasks));
		if (stStalled.iCode != OI_ERR_TIMEOUT || stHealthy.iCode != OI_OK || stHealthy.sBody != "ok")
		{
			LOG_ERROR("https stall failed timeout=%d health=%d", stStalled.iCode, stHealthy.iCode);
			++iBad;
		}
	}
	else if (sCase == "hostname" || sCase == "untrusted")
	{
		auto stResponse = co_await stClient.Get("/health");
		if (stResponse.iCode != OI_ERR_CONNECT || (sCase == "hostname" ? stResponse.sError.find("mismatch") == std::string::npos : stResponse.sError.find("certificate") == std::string::npos))
		{
			LOG_ERROR("https verification failed code=%d reason=%s", stResponse.iCode, stResponse.sError.c_str());
			++iBad;
		}
	}
	else
	{
		auto stFirst = co_await stClient.Get("/health");
		auto stSecond = co_await stClient.Get("/health");
		auto stTruncated = co_await stClient.Get("/truncated");
		auto stAfter = co_await stClient.Get("/health");
		OI::Http::Request stStreamRequest;
		stStreamRequest.sMethod = "POST";
		stStreamRequest.sPath = "/stream";
		stStreamRequest.pBodySource = std::make_shared<WebTestSource>(65536);
		auto pStreamSink = std::make_shared<WebTestSink>();
		stStreamRequest.pBodySink = pStreamSink;
		auto stStream = co_await stClient.Send(stStreamRequest);
		auto stCleanEof = co_await stClient.Get("/clean-eof");
		auto stBadEof = co_await stClient.Get("/bad-eof");
		if (stFirst.iCode != OI_OK || stFirst.sBody != "ok" || stSecond.iCode != OI_OK || stSecond.sBody != "ok" || stTruncated.iCode == OI_OK || stAfter.iCode != OI_OK || stAfter.sBody != "ok" || stStream.iCode != OI_OK || pStreamSink->Size() != 65536 || !pStreamSink->Valid() || stCleanEof.iCode != OI_OK || stCleanEof.sBody != "clean" || stBadEof.iCode == OI_OK)
		{
			LOG_ERROR("https roundtrip failed first=%d second=%d truncated=%d after=%d stream=%d clean=%d bad=%d", stFirst.iCode, stSecond.iCode, stTruncated.iCode, stAfter.iCode, stStream.iCode, stCleanEof.iCode, stBadEof.iCode);
			++iBad;
		}
	}
	LOG_TRACE("https regression case=%s bad=%d", sCase.c_str(), iBad);
	fflush(stdout);
	OI::Thread::ThreadPool::GetInstance().RequestStop(iBad == 0 ? 0 : 1);
}

OI::Coro::Task WebClient::Run()
{
	std::string sIpLocal = "127.0.0.1";
	int32_t iPortLocal = 18080;
	int64_t iTimeoutMilliseconds = 5000;
	bool bReuse = true;
	bool bHttps = false;
	std::string sCaFile;
	std::string sTlsCase;
	int32_t iStallPort = 0;
	int32_t iUrlTlsPort = 0;
	int32_t iPoolControlPort = 0;
	bool bUrlCase = false;
	bool bPoolCase = false;
	bool bCheckEchoHeader = false;
	int32_t iStreamFixturePort = 0;
	TryGetConfig("WebClient.pool_case", bPoolCase);
	TryGetConfig("WebClient.pool_control_port", iPoolControlPort);
	TryGetConfig("WebClient.url_case", bUrlCase);
	TryGetConfig("WebClient.url_tls_port", iUrlTlsPort);
	TryGetConfig("WebClient.https", bHttps);
	TryGetConfig("WebClient.ca_file", sCaFile);
	TryGetConfig("WebClient.tls_case", sTlsCase);
	TryGetConfig("WebClient.stall_port", iStallPort);
	TryGetConfig("WebClient.stream_fixture_port", iStreamFixturePort);
	TryGetConfig("WebClient.check_echo_header", bCheckEchoHeader);
	TryGetConfig("WebClient.ip", sIpLocal);
	TryGetConfig("WebClient.port", iPortLocal);
	TryGetConfig("WebClient.timeout_ms", iTimeoutMilliseconds);
	TryGetConfig("WebClient.reuse", bReuse);
	if (iTimeoutMilliseconds <= 0 || iTimeoutMilliseconds > INT32_MAX)
	{
		LOG_ERROR("invalid WebClient.timeout_ms=%lld", static_cast<long long>(iTimeoutMilliseconds));
		::OI::Thread::ThreadPool::GetInstance().RequestStop(OI_ERR_INVALID_ARGUMENT);
		co_return;
	}

	if (bPoolCase)
	{
		co_await RunPoolRegression(iPortLocal, iPoolControlPort);
		co_return;
	}
	if (bUrlCase)
	{
		co_await RunUrlRegression(iPortLocal, iUrlTlsPort, sCaFile);
		co_return;
	}
	if (!sTlsCase.empty())
	{
		co_await RunTlsRegression(sIpLocal, iPortLocal, sCaFile, sTlsCase, iStallPort);
		co_return;
	}
	std::optional<OI::Http::Client> stClient;
	if (bHttps)
	{
		stClient.emplace(sIpLocal, iPortLocal, OI::Http::TlsOptions{sCaFile}, static_cast<int32_t>(iTimeoutMilliseconds), bReuse);
	}
	else
	{
		stClient.emplace(sIpLocal, iPortLocal, static_cast<int32_t>(iTimeoutMilliseconds), bReuse);
	}
	auto& stCli = *stClient;
	int iBadLocal = 0;

	::OI::Http::Response stMissingQuery = co_await stCli.Get("/query");
	if (stMissingQuery.iCode != OI_OK || stMissingQuery.iStatus != 400)
	{
		LOG_ERROR("web missing query failed code=%d status=%d expected_status=400", stMissingQuery.iCode, stMissingQuery.iStatus);
		iBadLocal += 1;
	}
	::OI::Http::Response stHealth = co_await stCli.Get("/health");
	if (stHealth.iCode != OI_OK || stHealth.iStatus != 200 || stHealth.sBody != "ok")
	{
		LOG_ERROR("web health failed code=%d status=%d body_ok=%d", stHealth.iCode, stHealth.iStatus, stHealth.sBody == "ok" ? 1 : 0);
		iBadLocal += 1;
	}

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

	const std::vector<std::string> vQueryPaths = {"/query?existing=original", "/query?", "/query?existing=original&"};
	for (const auto& sPath : vQueryPaths)
	{
		::OI::Http::Response stQueryResponse = co_await stCli.Get(sPath, vQuery);
		Json::Value stQueryBody;
		if (OkStatus(stQueryResponse) != OI_OK || !ParseJson(stQueryResponse.sBody, stQueryBody) || !stQueryBody.isMember("text") || !stQueryBody["text"].isString() || stQueryBody["text"].asString() != sText || (sPath != "/query?" && (!stQueryBody.isMember("existing") || !stQueryBody["existing"].isString() || stQueryBody["existing"].asString() != "original")))
		{
			LOG_TRACE("web query path failed path=%s code=%d", sPath.c_str(), stQueryResponse.iCode);
			iBadLocal += 1;
		}
	}

	if (bCheckEchoHeader)
	{
		const std::vector<std::pair<std::string, std::string>> vRepeatQuery = {{"text", ""}, {"text", "a b+&"}};
		const std::vector<std::string> vExpectedTexts = {"path first", "", "", "a b+&"};
		auto fnCheckRepeat = [vExpectedTexts](const ::OI::Http::Response& stResponse)
		{
			Json::Value stBody;
			if (stResponse.iCode != OI_OK || stResponse.iStatus != 200 || !ParseJson(stResponse.sBody, stBody) ||
			    !stBody["text"].isString() || stBody["text"].asString() != vExpectedTexts[0] ||
			    !stBody["texts"].isArray() || stBody["texts"].size() != vExpectedTexts.size())
			{
				return false;
			}
			for (size_t iIndex = 0; iIndex < vExpectedTexts.size(); ++iIndex)
			{
				if (!stBody["texts"][static_cast<Json::ArrayIndex>(iIndex)].isString() ||
				    stBody["texts"][static_cast<Json::ArrayIndex>(iIndex)].asString() != vExpectedTexts[iIndex])
				{
					return false;
				}
			}
			return true;
		};
		::OI::Http::Response stRepeatedGet = co_await stCli.Get("/query?text=path%20first&text", vRepeatQuery);
		if (!fnCheckRepeat(stRepeatedGet))
		{
			LOG_ERROR("web repeated query Get failed code=%d status=%d", stRepeatedGet.iCode, stRepeatedGet.iStatus);
			++iBadLocal;
		}
		::OI::Http::Request stRepeatRequest;
		stRepeatRequest.sPath = "/query?text=path%20first&text";
		stRepeatRequest.mQuery.Add("text", "");
		stRepeatRequest.mQuery.Add("text", "a b+&");
		const std::string* pFirstText = stRepeatRequest.mQuery.Get("text");
		if (!pFirstText || !pFirstText->empty() || stRepeatRequest.mQuery.GetAll("text") != std::vector<std::string>{"", "a b+&"} ||
		    stRepeatRequest.mQuery.Get("missing") != nullptr || !stRepeatRequest.mQuery.GetAll("missing").empty())
		{
			LOG_ERROR("web query accessors failed");
			++iBadLocal;
		}
		::OI::Http::Response stRepeatedSend = co_await stCli.Send(stRepeatRequest);
		if (!fnCheckRepeat(stRepeatedSend))
		{
			LOG_ERROR("web repeated query Send failed code=%d status=%d", stRepeatedSend.iCode, stRepeatedSend.iStatus);
			++iBadLocal;
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
	::OI::Http::Request stRpcRequest;
	stRpcRequest.sMethod = "POST";
	stRpcRequest.sPath = "/rpcjson";
	if (stRpcRequest.SetJson(stSent) != OI_OK)
	{
		LOG_ERROR("web rpcdata JSON encoding failed");
		::OI::Thread::ThreadPool::GetInstance().RequestStop(OI_ERR_ENCODE);
		co_return;
	}
	::OI::Http::Response stRpc = co_await stCli.Send(stRpcRequest);
	int iRpcOk = 0;
	if (OkStatus(stRpc) != OI_OK)
	{
		iBadLocal += 1;
	}
	else
	{
		::OI::RpcData::JsonAll stBack;
		// clang-format off
		bool bSame = PlainJsonKeys(stRpcRequest.sBody)
			&& PlainJsonKeys(stRpc.sBody)
			&& stRpc.GetJson(stBack) == OI_OK
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

	if (bCheckEchoHeader)
	{
		if (!WebJsonRegression::Check())
		{
			LOG_ERROR("web JSON value and error checks failed");
			++iBadLocal;
		}
		OI::Http::Request stStreamRequest;
		stStreamRequest.sMethod = "POST";
		stStreamRequest.sPath = "/test-stream";
		stStreamRequest.pBodySource = std::make_shared<WebTestSource>(9 * 1024 * 1024);
		auto pStreamSink = std::make_shared<WebTestSink>();
		stStreamRequest.pBodySink = pStreamSink;
		OI::Http::Response stStreamResponse = co_await stCli.Send(stStreamRequest);
		if (stStreamResponse.iCode != OI_OK || stStreamResponse.iStatus != 200 || !pStreamSink->Valid() || pStreamSink->Size() != 9 * 1024 * 1024)
		{
			LOG_ERROR("web streaming roundtrip failed code=%d status=%d bytes=%zu", stStreamResponse.iCode, stStreamResponse.iStatus, pStreamSink->Size());
			++iBadLocal;
		}
		if (iStreamFixturePort <= 0)
		{
			LOG_ERROR("web stream fixture port missing");
			++iBadLocal;
		}
		else
		{
			OI::Http::Client stFixtureClient("127.0.0.1", iStreamFixturePort, 120000, false);
			for (const auto& sPath : std::vector<std::string>{"/length", "/eof"})
			{
				OI::Http::Request stFixtureRequest;
				stFixtureRequest.sPath = sPath;
				auto pFixtureSink = std::make_shared<WebTestSink>();
				stFixtureRequest.pBodySink = pFixtureSink;
				OI::Http::Response stFixtureResponse = co_await stFixtureClient.Send(stFixtureRequest);
				if (stFixtureResponse.iCode != OI_OK || stFixtureResponse.iStatus != 200 ||
				    !pFixtureSink->Valid() || pFixtureSink->Size() != 9 * 1024 * 1024)
				{
					LOG_ERROR("web stream fixture failed path=%s code=%d status=%d bytes=%zu", sPath.c_str(), stFixtureResponse.iCode, stFixtureResponse.iStatus, pFixtureSink->Size());
					++iBadLocal;
				}
			}
		}
		OI::Http::Client stShortClient(sIpLocal, iPortLocal, 500, true);
		OI::Http::Request stTimeoutRequest;
		stTimeoutRequest.sMethod = "POST";
		stTimeoutRequest.sPath = "/test-stream";
		stTimeoutRequest.pBodySource = std::make_shared<WebTestBlockedSource>();
		OI::Http::Response stTimeoutResponse = co_await stShortClient.Send(stTimeoutRequest);
		if (stTimeoutResponse.iCode != OI_ERR_TIMEOUT)
		{
			LOG_ERROR("web stream timeout failed code=%d", stTimeoutResponse.iCode);
			++iBadLocal;
		}
		OI::Http::Request stMidCancelRequest;
		stMidCancelRequest.sMethod = "POST";
		stMidCancelRequest.sPath = "/test-stream";
		stMidCancelRequest.pBodySource = std::make_shared<WebTestSource>(9 * 1024 * 1024);
		auto pCancelSink = std::make_shared<WebTestCancelSink>();
		stMidCancelRequest.pBodySink = pCancelSink;
		OI::Http::Response stMidCancelResponse = co_await stCli.Send(stMidCancelRequest);
		if (stMidCancelResponse.iCode != OI_ERR_CANCELLED || pCancelSink->Size() == 0)
		{
			LOG_ERROR("web midstream cancellation failed code=%d bytes=%zu", stMidCancelResponse.iCode, pCancelSink->Size());
			++iBadLocal;
		}
		OI::Http::Request stCancelledRequest;
		stCancelledRequest.sMethod = "POST";
		stCancelledRequest.sPath = "/test-stream";
		stCancelledRequest.pBodySource = std::make_shared<WebTestSource>(9 * 1024 * 1024);
		stCancelledRequest.pBodySource->Cancel();
		OI::Http::Response stCancelledResponse = co_await stCli.Send(stCancelledRequest);
		if (stCancelledResponse.iCode != OI_ERR_CANCELLED)
		{
			LOG_ERROR("web stream cancellation failed code=%d", stCancelledResponse.iCode);
			++iBadLocal;
		}
		OI::Http::Response stAfterStream = co_await stCli.Get("/health");
		if (stAfterStream.iCode != OI_OK || stAfterStream.iStatus != 200 || stAfterStream.sBody != "ok")
		{
			LOG_ERROR("web after stream cancellation failed code=%d status=%d", stAfterStream.iCode, stAfterStream.iStatus);
			++iBadLocal;
		}
		OI::Http::QueryParams stForm;
		stForm.Add("repeat", "first");
		stForm.Add("repeat", "a b+&");
		stForm.Add("empty", "");
		OI::Http::Request stFormRequest;
		stFormRequest.sMethod = "POST";
		stFormRequest.sPath = "/test-form";
		if (stFormRequest.SetForm(stForm) != OI_OK)
		{
			LOG_ERROR("web form encoding failed");
			++iBadLocal;
		}
		OI::Http::Response stFormResponse = co_await stCli.Send(stFormRequest);
		OI::Http::QueryParams stFormBack;
		const int32_t iFormCode = stFormResponse.GetForm(stFormBack);
		const std::string* pEmpty = stFormBack.Get("empty");
		if (stFormResponse.iCode != OI_OK || stFormResponse.iStatus != 200 || iFormCode != OI_OK ||
		    stFormBack.GetAll("repeat") != std::vector<std::string>{"first", "a b+&"} || !pEmpty || !pEmpty->empty())
		{
			LOG_ERROR("web form roundtrip failed code=%d status=%d parse=%d", stFormResponse.iCode, stFormResponse.iStatus, iFormCode);
			++iBadLocal;
		}
		OI::Http::Response stBadForm;
		stBadForm.sContentType = "application/x-www-form-urlencoded";
		stBadForm.sBody = "name=%GG";
		OI::Http::QueryParams stKeptForm;
		stKeptForm.Add("sentinel", "original");
		const int32_t iBadFormCode = stBadForm.GetForm(stKeptForm);
		const std::string* pKeptForm = stKeptForm.Get("sentinel");
		if (iBadFormCode == OI_OK || !pKeptForm || *pKeptForm != "original" || stKeptForm.Get("name"))
		{
			LOG_ERROR("web malformed form did not preserve output code=%d", iBadFormCode);
			++iBadLocal;
		}
		OI::Http::Response stBadMultipart;
		stBadMultipart.sContentType = "multipart/form-data; boundary=boundary42";
		stBadMultipart.sBody = "--boundary42\r\nContent-Disposition: form-data; name=\"field\"\r\n\r\nunfinished";
		std::vector<OI::Http::FormPart> vKeptParts(1);
		vKeptParts[0].sName = "sentinel";
		vKeptParts[0].sValue = "original";
		const int32_t iBadMultipartCode = stBadMultipart.GetMultipart(vKeptParts);
		if (iBadMultipartCode == OI_OK || vKeptParts.size() != 1 || vKeptParts[0].sName != "sentinel" || vKeptParts[0].sValue != "original")
		{
			LOG_ERROR("web incomplete multipart did not preserve output code=%d", iBadMultipartCode);
			++iBadLocal;
		}
		std::vector<OI::Http::FormPart> vParts(4);
		vParts[0].sName = "repeat";
		vParts[0].sValue = "first";
		vParts[1].sName = "repeat";
		vParts[2].sName = "files";
		vParts[2].sFilename = "one.bin";
		vParts[2].sContentType = "application/octet-stream";
		vParts[2].sValue = std::string("\0\1\xff", 3);
		vParts[3].sName = "files";
		vParts[3].sFilename = "two.bin";
		vParts[3].sContentType = "application/octet-stream";
		vParts[3].sValue = std::string("second\0file", 11);
		OI::Http::Request stMultipartRequest;
		stMultipartRequest.sMethod = "POST";
		stMultipartRequest.sPath = "/test-multipart";
		if (stMultipartRequest.SetMultipart(vParts) != OI_OK)
		{
			LOG_ERROR("web multipart encoding failed");
			++iBadLocal;
		}
		OI::Http::Response stMultipartResponse = co_await stCli.Send(stMultipartRequest);
		std::vector<OI::Http::FormPart> vBack;
		const int32_t iMultipartCode = stMultipartResponse.GetMultipart(vBack);
		if (stMultipartResponse.iCode != OI_OK || stMultipartResponse.iStatus != 200 || iMultipartCode != OI_OK || vBack.size() != vParts.size())
		{
			LOG_ERROR("web multipart roundtrip failed code=%d status=%d parse=%d count=%zu", stMultipartResponse.iCode, stMultipartResponse.iStatus, iMultipartCode, vBack.size());
			++iBadLocal;
		}
		else
		{
			for (size_t iIndex = 0; iIndex < vParts.size(); ++iIndex)
			{
				if (vParts[iIndex].sName != vBack[iIndex].sName || vParts[iIndex].sValue != vBack[iIndex].sValue ||
				    vParts[iIndex].sFilename != vBack[iIndex].sFilename || vParts[iIndex].sContentType != vBack[iIndex].sContentType)
				{
					LOG_ERROR("web multipart mismatch index=%zu", iIndex);
					++iBadLocal;
				}
			}
		}
	}

	if (bCheckEchoHeader)
	{
		::OI::Http::Request stInvalidRequest;
		stInvalidRequest.sPath = "/health";
		stInvalidRequest.vHeaders.emplace_back("Content-Length", "0");
		::OI::Http::Response stInvalidResponse = co_await stCli.Send(stInvalidRequest);
		if (stInvalidResponse.iCode != OI_ERR_INVALID_ARGUMENT || stInvalidResponse.iStatus != 0)
		{
			LOG_ERROR("web invalid header check failed code=%d status=%d", stInvalidResponse.iCode, stInvalidResponse.iStatus);
			++iBadLocal;
		}
	}

	std::string sBin("\x00\x01\xff", 3);
	::OI::Http::Request stBinaryRequest;
	stBinaryRequest.sMethod = "POST";
	stBinaryRequest.sPath = "/bin";
	stBinaryRequest.sBody = sBin;
	stBinaryRequest.sContentType = "application/octet-stream";
	stBinaryRequest.vHeaders.emplace_back("X-Echo-Tag", "sample-tag");
	const std::string sCookieBytes("a b;\x00\xff", 6);
	const std::string sCookieEncoded = ::OI::Http::Cookie::EncodeValue(sCookieBytes);
	if (bCheckEchoHeader)
	{
		if (stBinaryRequest.AddCookie("token", "first") != OI_OK ||
		    stBinaryRequest.AddCookie("token", "second") != OI_OK ||
		    stBinaryRequest.AddCookie("Token", "case") != OI_OK ||
		    stBinaryRequest.AddCookie("empty", "") != OI_OK ||
		    stBinaryRequest.AddCookie("encoded", sCookieEncoded) != OI_OK)
		{
			LOG_ERROR("web cookie request construction failed");
			++iBadLocal;
		}
		std::vector<std::pair<std::string, std::string>> vRequestCookies;
		const std::vector<std::pair<std::string, std::string>> vExpectedCookies = {
		    {"token", "first"}, {"token", "second"}, {"Token", "case"}, {"empty", ""}, {"encoded", sCookieEncoded}};
		if (stBinaryRequest.GetCookies(vRequestCookies) != OI_OK || vRequestCookies != vExpectedCookies)
		{
			LOG_ERROR("web request cookie parsing failed");
			++iBadLocal;
		}
		const auto vHeadersBefore = stBinaryRequest.vHeaders;
		if (stBinaryRequest.AddCookie("invalid name", "ok") == OI_OK ||
		    stBinaryRequest.AddCookie("invalid", "bad;value") == OI_OK || stBinaryRequest.vHeaders != vHeadersBefore)
		{
			LOG_ERROR("web invalid cookie request accepted or changed headers");
			++iBadLocal;
		}
	}
	::OI::Http::Response stRaw = co_await stCli.Send(stBinaryRequest);
	const std::string* pEchoTag = stRaw.GetHeader("x-echo-tag");
	int iBinOk = 0;
	if (OkStatus(stRaw) == OI_OK && stRaw.sBody.size() == sBin.size() && std::memcmp(stRaw.sBody.data(), sBin.data(), sBin.size()) == 0 &&
	    (!bCheckEchoHeader || (pEchoTag && *pEchoTag == "sample-tag")))
	{
		iBinOk = 1;
	}
	else
	{
		iBadLocal += 1;
	}

	if (bCheckEchoHeader)
	{
		std::vector<::OI::Http::Cookie> vCookies;
		const int32_t iCookieCode = stRaw.GetCookies(vCookies);
		if (iCookieCode != OI_OK || vCookies.size() != 5)
		{
			LOG_ERROR("web cookie response count failed code=%d count=%zu", iCookieCode, vCookies.size());
			++iBadLocal;
		}
		else
		{
			const std::vector<std::pair<std::string, std::string>> vExpected = {
			    {"token", "first"}, {"token", "second"}, {"Token", "case"}, {"empty", ""}, {"encoded", sCookieEncoded}};
			for (size_t iIndex = 0; iIndex < vCookies.size(); ++iIndex)
			{
				const auto& stCookie = vCookies[iIndex];
				if (stCookie.sName != vExpected[iIndex].first || stCookie.sValue != vExpected[iIndex].second ||
				    stCookie.sPath != "/" || stCookie.sDomain != "example.test" || stCookie.sExpires != "Wed, 09 Jun 2027 10:18:14 GMT" ||
				    !stCookie.iMaxAge.has_value() || *stCookie.iMaxAge != 0 || !stCookie.bHttpOnly || !stCookie.bSecure || stCookie.sSameSite != "Lax")
				{
					LOG_ERROR("web cookie response mismatch index=%zu", iIndex);
					++iBadLocal;
				}
			}
			std::string sDecoded;
			if (::OI::Http::Cookie::DecodeValue(vCookies[4].sValue, sDecoded) != OI_OK || sDecoded != sCookieBytes)
			{
				LOG_ERROR("web cookie encoded value roundtrip failed");
				++iBadLocal;
			}
		}
		::OI::Http::Response stMixedCase;
		stMixedCase.vHeaders.emplace_back("sEt-CoOkIe", "Mixed=value; pAtH=/; dOmAiN=example.test; eXpIrEs=Wed, 09 Jun 2027 10:18:14 GMT; mAx-AgE=0; hTtPoNlY; sEcUrE; sAmEsItE=Lax");
		std::vector<::OI::Http::Cookie> vMixed;
		if (stMixedCase.GetCookies(vMixed) != OI_OK || vMixed.size() != 1 || vMixed[0].sName != "Mixed" ||
		    vMixed[0].sPath != "/" || vMixed[0].sDomain != "example.test" || vMixed[0].sExpires != "Wed, 09 Jun 2027 10:18:14 GMT" ||
		    !vMixed[0].iMaxAge.has_value() || *vMixed[0].iMaxAge != 0 || !vMixed[0].bSecure || !vMixed[0].bHttpOnly || vMixed[0].sSameSite != "Lax")
		{
			LOG_ERROR("web mixed case cookie attributes failed");
			++iBadLocal;
		}
		::OI::Http::Request stBadRequestCookie;
		stBadRequestCookie.vHeaders.emplace_back("Cookie", "bad=bad value");
		std::vector<std::pair<std::string, std::string>> vPreservedRequest = {{"sentinel", "value"}};
		if (stBadRequestCookie.GetCookies(vPreservedRequest) == OI_OK ||
		    vPreservedRequest != std::vector<std::pair<std::string, std::string>>{{"sentinel", "value"}})
		{
			LOG_ERROR("web invalid request cookie did not preserve output");
			++iBadLocal;
		}
		::OI::Http::Response stBadCookie;
		::OI::Http::Cookie stInvalidCookie;
		stInvalidCookie.sName = "bad name";
		stInvalidCookie.sValue = "value";
		if (stBadCookie.AddCookie(stInvalidCookie) == OI_OK || !stBadCookie.vHeaders.empty())
		{
			LOG_ERROR("web invalid response cookie accepted or changed headers");
			++iBadLocal;
		}
		stBadCookie.vHeaders.emplace_back("Set-Cookie", "bad=bad value");
		std::vector<::OI::Http::Cookie> vPreserved(1);
		vPreserved[0].sName = "sentinel";
		std::string sPreserved = "sentinel";
		if (stBadCookie.GetCookies(vPreserved) == OI_OK || vPreserved.size() != 1 || vPreserved[0].sName != "sentinel" ||
		    ::OI::Http::Cookie::DecodeValue("%GG", sPreserved) == OI_OK || sPreserved != "sentinel")
		{
			LOG_ERROR("web invalid cookie decode did not preserve output");
			++iBadLocal;
		}
		::OI::Http::Request stPlainRequest;
		stPlainRequest.sMethod = "POST";
		stPlainRequest.sPath = "/bin";
		stPlainRequest.vHeaders.emplace_back("X-Echo-Tag", "sample-tag");
		::OI::Http::Response stPlain = co_await stCli.Send(stPlainRequest);
		std::vector<::OI::Http::Cookie> vPlain;
		if (stPlain.iCode != OI_OK || stPlain.iStatus != 200 || stPlain.GetCookies(vPlain) != OI_OK || !vPlain.empty())
		{
			LOG_ERROR("web cookie jar isolation failed code=%d status=%d count=%zu", stPlain.iCode, stPlain.iStatus, vPlain.size());
			++iBadLocal;
		}
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
