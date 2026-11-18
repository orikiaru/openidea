#include "web_server.h"
#include "oi_http_json.h"
#include "web_stream_fixture.h"
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
using OI::Http::Request;
using OI::Http::Response;

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

OI::Coro::Task Health(const Request&, Response& stReply)
{
	stReply.sBody = "ok";
	co_return;
}

OI::Coro::Task JsonEcho(const Request& stRequest, Response& stReply)
{
	Json::Value stBody;
	if (stRequest.GetJson(stBody) != OI_OK || !stBody.isObject())
	{
		stReply.iStatus = 400;
		co_return;
	}
	stBody["ok"] = true;
	if (stReply.SetJson(stBody) != OI_OK)
	{
		stReply.iStatus = 500;
	}
}

OI::Coro::Task RpcJson(const Request& stRequest, Response& stReply)
{
	OI::RpcData::JsonAll stData;
	if (stRequest.GetJson(stData) != OI_OK || stData.iGap == INT32_MAX)
	{
		stReply.iStatus = 400;
		co_return;
	}
	++stData.iGap;
	if (stReply.SetJson(stData) != OI_OK)
	{
		stReply.iStatus = 500;
	}
}

OI::Coro::Task FormEcho(const Request& stRequest, Response& stReply)
{
	OI::Http::QueryParams stForm;
	if (stRequest.GetForm(stForm) != OI_OK || stReply.SetForm(stForm) != OI_OK)
	{
		stReply.iStatus = 400;
	}
	co_return;
}

class DisconnectTestSink : public WebTestSink
{
  public:
	~DisconnectTestSink() override
	{
		std::printf("web disconnect sink released cancelled=%d\n", IsCancelled() ? 1 : 0);
		std::fflush(stdout);
	}
};

std::shared_ptr<OI::Http::BodySink> MakeStreamSink(const Request& stRequest)
{
	if (stRequest.GetHeader("x-disconnect-check"))
	{
		std::puts("web disconnect sink created");
		std::fflush(stdout);
		return std::make_shared<DisconnectTestSink>();
	}
	return std::make_shared<WebTestSink>();
}

OI::Coro::Task StreamEcho(const Request& stRequest, Response& stReply)
{
	const auto pSink = std::static_pointer_cast<WebTestSink>(stRequest.pBodySink);
	if (!pSink || !pSink->Valid() || pSink->Size() != 9 * 1024 * 1024)
	{
		stReply.iStatus = 400;
		co_return;
	}
	stReply.sContentType = "application/octet-stream";
	stReply.pBodySource = std::make_shared<WebTestSource>(9 * 1024 * 1024);
}

OI::Coro::Task MultipartEcho(const Request& stRequest, Response& stReply)
{
	std::vector<OI::Http::FormPart> vParts;
	if (stRequest.GetMultipart(vParts) != OI_OK || stReply.SetMultipart(vParts) != OI_OK)
	{
		stReply.iStatus = 400;
	}
	co_return;
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
	if (stReply.SetJson(stBody) != OI_OK)
	{
		stReply.iStatus = 500;
		co_return;
	}
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
	bool bFormRegression = false;
	TryGetConfig("WebServer.form_regression", bFormRegression);
	if (bFormRegression)
	{
		Register("POST", "/test-form", FormEcho);
		Register("POST", "/test-multipart", MultipartEcho);
		RegisterStream("POST", "/test-stream", MakeStreamSink, StreamEcho);
	}
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
