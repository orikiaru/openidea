#include "web_server.h"
#include "oi_config.h"
#include "oi_rpc_call.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "oi_rpcdata_jsonall.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "json/json.h"
#include <cstdio>
#include <cstring>

namespace
{
using OI::Http::Response;
using OI::Http::Request;

class WebRpc : public OI::Rpc::ProtoClientManager
{
	DECLARE_SINGLETON(WebRpc)
	WebRpc() = default;
	const std::string GetIDString() override
	{
		return "WebRpc";
	}
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
	}
};

void SetJson(const Json::Value& stValue, Response& stReply)
{
	Json::StreamWriterBuilder stBuilder;
	stBuilder["indentation"] = "";
	stReply.sContentType = "application/json";
	stReply.sBody = Json::writeString(stBuilder, stValue);
}

OI::Coro::Task Health(const Request&, Response& stReply)
{
	stReply.sBody = "ok";
	co_return;
}

OI::Coro::Task JsonEcho(const Request& stRequest, Response& stReply)
{
	Json::CharReaderBuilder stBuilder;
	std::unique_ptr<Json::CharReader> pReader(stBuilder.newCharReader());
	Json::Value stBody;
	std::string sErrors;
	const char* pBegin = stRequest.sBody.data();
	if (!pReader->parse(pBegin, pBegin + stRequest.sBody.size(), &stBody, &sErrors) || !stBody.isObject())
	{
		stReply.iStatus = 400;
		co_return;
	}
	stBody["ok"] = true;
	SetJson(stBody, stReply);
}

OI::Coro::Task RpcJson(const Request& stRequest, Response& stReply)
{
	OI::RpcData::JsonAll stData;
	if (!stData.FromJson(stRequest.sBody) || stData.iGap == INT32_MAX)
	{
		stReply.iStatus = 400;
		co_return;
	}
	++stData.iGap;
	stReply.sBody = stData.ToJson();
	stReply.sContentType = "application/json";
}

}

OI::Coro::Task WebServer::Binary(const Request& stRequest, Response& stReply)
{
	if (const auto* pTag = stRequest.GetHeader("x-echo-tag"))
	{
		stReply.vHeaders.emplace_back("X-Echo-Tag", *pTag);
	}
	if (GetInstance().m_bEchoCookies)
	{
		std::vector<std::pair<std::string, std::string>> vPairs;
		if (stRequest.GetCookies(vPairs) != OI_OK)
		{
			stReply.iStatus = 400;
			co_return;
		}
		for (const auto& [sName, sValue] : vPairs)
		{
			OI::Http::Cookie stCookie;
			stCookie.sName = sName;
			stCookie.sValue = sValue;
			stCookie.sPath = "/";
			stCookie.sDomain = "example.test";
			stCookie.sExpires = "Wed, 09 Jun 2027 10:18:14 GMT";
			stCookie.iMaxAge = 0;
			stCookie.bSecure = true;
			stCookie.bHttpOnly = true;
			stCookie.sSameSite = "Lax";
			if (stReply.AddCookie(stCookie) != OI_OK)
			{
				stReply.vHeaders.clear();
				stReply.iStatus = 500;
				co_return;
			}
		}
	}
	stReply.sBody = stRequest.sBody;
	stReply.sContentType = "application/octet-stream";
	co_return;
}

OI::Coro::Task WebServer::Query(const Request& stRequest, Response& stReply)
{
	const auto vText = stRequest.mQuery.GetAll("text");
	if (vText.empty())
	{
		stReply.iStatus = 400;
		co_return;
	}
	auto& stServer = GetInstance();
	if (stServer.m_bRpc)
	{
		OI::RpcData::HoldReq stCall;
		OI::RpcData::HoldRes stResult;
		stCall.iMark = 1;
		stCall.iDelayMilliseconds = stServer.m_iDelayMilliseconds;
		if (stServer.m_iDelayMilliseconds > 0)
		{
			std::puts("web rpc begin");
			std::fflush(stdout);
		}
		const int32_t iCode = co_await OI::Rpc::RpcCall(WebRpc::GetInstance(), stCall, stResult, OI::Timer::Seconds(5));
		if (iCode != OI_OK || stResult.iMark != stCall.iMark)
		{
			std::fprintf(stderr, "web upstream RPC failed code=%d\n", iCode);
			stReply.iStatus = 502;
			co_return;
		}
	}
	if (stServer.m_bRedis)
	{
		std::vector<std::string> vCommand{"PING"};
		auto stResult = co_await OI::Redis::CallRedis(std::move(vCommand));
		if (stResult.GetRet() != OI_OK || !stResult.pReply || stResult.pReply->type != REDIS_REPLY_STATUS ||
		    !stResult.pReply->str || std::strcmp(stResult.pReply->str, "PONG") != 0)
		{
			std::fprintf(stderr, "web upstream Redis PING failed code=%d\n", stResult.GetRet());
			stReply.iStatus = 502;
			co_return;
		}
	}
	Json::Value stBody(Json::objectValue);
	stBody["text"] = vText.front();
	if (vText.size() > 1)
	{
		for (const auto& sText : vText)
		{
			stBody["texts"].append(sText);
		}
	}
	if (auto it = stRequest.mQuery.find("existing"); it != stRequest.mQuery.end())
	{
		stBody["existing"] = it->second;
	}
	SetJson(stBody, stReply);
	if (stServer.m_bRpc && stServer.m_iDelayMilliseconds > 0)
	{
		std::puts("web handler complete");
		std::fflush(stdout);
	}
}

int32_t WebServer::Init()
{
	std::string sIp = "127.0.0.1";
	int32_t iPort = 18080;
	TryGetConfig("WebServer.ip", sIp);
	TryGetConfig("WebServer.port", iPort);
	TryGetConfig("WebServer.echo_cookies", m_bEchoCookies);
	if (iPort <= 0 || iPort > 65535)
	{
		std::fprintf(stderr, "web server bad port=%d\n", iPort);
		return OI_ERR_INVALID_ARGUMENT;
	}
	DefaultInit(sIp, std::to_string(iPort));
	int32_t iCode = LoadSessionLimits();
	if (iCode != OI_OK)
	{
		return iCode;
	}
	Register("GET", "/health", Health);
	Register("GET", "/query", Query);
	Register("POST", "/json", JsonEcho);
	Register("POST", "/rpcjson", RpcJson);
	Register("POST", "/bin", Binary);
	std::string sPort;
	m_bRpc = TryGetConfig("WebRpc.port", sPort);
	if (m_bRpc)
	{
		TryGetConfig("WebRpc.delay_ms", m_iDelayMilliseconds);
		iCode = WebRpc::GetInstance().Init();
		if (iCode != OI_OK)
		{
			return iCode;
		}
		WebRpc::GetInstance().StartClient();
	}
	m_bRedis = TryGetConfig("RedisClient.port", sPort);
	if (m_bRedis)
	{
		iCode = OI::Redis::RedisClient::GetInstance().Init();
		if (iCode != OI_OK)
		{
			return iCode;
		}
		OI::Redis::RedisClient::GetInstance().StartClient();
	}
	return OI_OK;
}
