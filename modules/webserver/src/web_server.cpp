#include "web_server.h"
#include "oi_config.h"
#include "oi_rpcdata_jsonall.h"
#include "httplib.h"
#include "json/json.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

static std::string DumpJson(const Json::Value& stValueParam)
{
	Json::StreamWriterBuilder stBuilder;
	stBuilder["indentation"] = "";
	return Json::writeString(stBuilder, stValueParam);
}

static bool ParseJson(const std::string& sTextParam, Json::Value& stOutputParam)
{
	Json::CharReaderBuilder stBuilder;
	std::string sErrors;
	const std::unique_ptr<Json::CharReader> pReader(stBuilder.newCharReader());
	const char* pBegin = sTextParam.data();
	return pReader->parse(pBegin, pBegin + sTextParam.size(), &stOutputParam, &sErrors);
}

bool WebServer::Run()
{
	std::string sIpLocal = "127.0.0.1";
	int iPortLocal = 18080;
	TryGetConfig("WebServer.ip", sIpLocal);
	TryGetConfig("WebServer.port", iPortLocal);
	if (iPortLocal <= 0 || iPortLocal > 65535)
	{
		printf("web server bad port=%d\n", iPortLocal);
		return false;
	}

	httplib::Server stSvr;
	stSvr.Get("/health", [](const httplib::Request&, httplib::Response& stResponseParam) {
		stResponseParam.set_content("ok", "text/plain");
	});
	stSvr.Get("/query", [](const httplib::Request& stRequestParam, httplib::Response& stResponseParam) {
		if (!stRequestParam.has_param("text"))
		{
			stResponseParam.status = 400;
			return;
		}
		Json::Value stOutput(Json::objectValue);
		stOutput["text"] = stRequestParam.get_param_value("text");
		stResponseParam.set_content(DumpJson(stOutput), "application/json");
	});
	stSvr.Post("/json", [](const httplib::Request& stRequestParam, httplib::Response& stResponseParam) {
		Json::Value stBody;
		if (!ParseJson(stRequestParam.body, stBody) || !stBody.isObject())
		{
			stResponseParam.status = 400;
			return;
		}
		stBody["ok"] = true;
		stResponseParam.set_content(DumpJson(stBody), "application/json");
	});
	stSvr.Post("/rpcjson", [](const httplib::Request& stRequestParam, httplib::Response& stResponseParam) {
		::OI::RpcData::JsonAll stDataLocal;
		if (!stDataLocal.FromJson(stRequestParam.body))
		{
			stResponseParam.status = 400;
			return;
		}
		stDataLocal.iGap += 1;
		stResponseParam.set_content(stDataLocal.ToJson(), "application/json");
	});
	stSvr.Post("/bin", [](const httplib::Request& stRequestParam, httplib::Response& stResponseParam) {
		stResponseParam.set_content(stRequestParam.body.data(), stRequestParam.body.size(), "application/octet-stream");
	});

	printf("web server listen %s:%d\n", sIpLocal.c_str(), iPortLocal);
	fflush(stdout);
	return stSvr.listen(sIpLocal, iPortLocal);
}
