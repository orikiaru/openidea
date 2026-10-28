#include "web_server.h"
#include "oi_config.h"
#include "oi_rpcdata_jsonall.h"
#include "httplib.h"
#include "json/json.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

static std::string DumpJson(const Json::Value& value)
{
	Json::StreamWriterBuilder builder;
	builder["indentation"] = "";
	return Json::writeString(builder, value);
}

static bool ParseJson(const std::string& text, Json::Value& out)
{
	Json::CharReaderBuilder builder;
	std::string errs;
	const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	const char* begin = text.data();
	return reader->parse(begin, begin + text.size(), &out, &errs);
}

bool WebServer::Run()
{
	std::string ip = "127.0.0.1";
	int port = 18080;
	TryGetConfig("WebServer.ip", ip);
	TryGetConfig("WebServer.port", port);
	if (port <= 0 || port > 65535)
	{
		printf("web server bad port=%d\n", port);
		return false;
	}

	httplib::Server svr;
	svr.Get("/health", [](const httplib::Request&, httplib::Response& res) {
		res.set_content("ok", "text/plain");
	});
	svr.Get("/query", [](const httplib::Request& req, httplib::Response& res) {
		if (!req.has_param("text"))
		{
			res.status = 400;
			return;
		}
		Json::Value out(Json::objectValue);
		out["text"] = req.get_param_value("text");
		res.set_content(DumpJson(out), "application/json");
	});
	svr.Post("/json", [](const httplib::Request& req, httplib::Response& res) {
		Json::Value body;
		if (!ParseJson(req.body, body) || !body.isObject())
		{
			res.status = 400;
			return;
		}
		body["ok"] = true;
		res.set_content(DumpJson(body), "application/json");
	});
	svr.Post("/rpcjson", [](const httplib::Request& req, httplib::Response& res) {
		OI::RpcData::JsonAll data;
		if (!data.FromJson(req.body))
		{
			res.status = 400;
			return;
		}
		data.gap += 1;
		res.set_content(data.ToJson(), "application/json");
	});
	svr.Post("/bin", [](const httplib::Request& req, httplib::Response& res) {
		res.set_content(req.body.data(), req.body.size(), "application/octet-stream");
	});

	printf("web server listen %s:%d\n", ip.c_str(), port);
	fflush(stdout);
	return svr.listen(ip, port);
}
