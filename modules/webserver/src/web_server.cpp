#include "web_server.h"
#include "oi_config.h"
#include "oi_errno.h"
#include "oi_rpcdata_jsonall.h"
#include "httplib.h"
#include "json/json.h"
#include <cstdio>
#include <cerrno>
#include <cstdlib>
#include <memory>
#include <string>

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

int32_t WebServer::Run()
{
	std::string sIpLocal = "127.0.0.1";
	int iPortLocal = 18080;
	TryGetConfig("WebServer.ip", sIpLocal);
	TryGetConfig("WebServer.port", iPortLocal);
	if (iPortLocal <= 0 || iPortLocal > 65535)
	{
		fprintf(stderr, "web server bad port=%d\n", iPortLocal);
		return OI_ERR_INVALID_ARGUMENT;
	}

	httplib::Server stSvr;
	stSvr.Get("/health", [](const httplib::Request&, httplib::Response& stResponse)
	{
		stResponse.set_content("ok", "text/plain");
	});
	stSvr.Get("/query", [](const httplib::Request& stRequest, httplib::Response& stResponse)
	{
		if (!stRequest.has_param("text"))
		{
			stResponse.status = 400;
			return;
		}
		Json::Value stOutput(Json::objectValue);
		stOutput["text"] = stRequest.get_param_value("text");
		stResponse.set_content(DumpJson(stOutput), "application/json");
	});
	stSvr.Post("/json", [](const httplib::Request& stRequest, httplib::Response& stResponse)
	{
		Json::Value stBody;
		if (!ParseJson(stRequest.body, stBody) || !stBody.isObject())
		{
			stResponse.status = 400;
			return;
		}
		stBody["ok"] = true;
		stResponse.set_content(DumpJson(stBody), "application/json");
	});
	stSvr.Post("/rpcjson", [](const httplib::Request& stRequest, httplib::Response& stResponse)
	{
		::OI::RpcData::JsonAll stDataLocal;
		if (!stDataLocal.FromJson(stRequest.body))
		{
			stResponse.status = 400;
			return;
		}
		stDataLocal.iGap += 1;
		stResponse.set_content(stDataLocal.ToJson(), "application/json");
	});
	stSvr.Post("/bin", [](const httplib::Request& stRequest, httplib::Response& stResponse)
	{
		stResponse.set_content(stRequest.body.data(), stRequest.body.size(), "application/octet-stream");
	});

	if (!stSvr.bind_to_port(sIpLocal, iPortLocal))
	{
#ifdef _WIN32
		int iSystemError = WSAGetLastError();
#else
		int iSystemError = errno;
#endif
		fprintf(stderr, "web server bind/listen failed %s:%d system_error=%d\n", sIpLocal.c_str(), iPortLocal, iSystemError);
		return OI_ERR_LISTEN;
	}
	printf("web server listen %s:%d\n", sIpLocal.c_str(), iPortLocal);
	fflush(stdout);
	if (!stSvr.listen_after_bind())
	{
#ifdef _WIN32
		int iSystemError = WSAGetLastError();
#else
		int iSystemError = errno;
#endif
		fprintf(stderr, "web server listen loop failed %s:%d system_error=%d\n", sIpLocal.c_str(), iPortLocal, iSystemError);
		return OI_ERR_LISTEN;
	}
	return OI_OK;
}
