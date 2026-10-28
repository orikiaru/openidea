#include "web_server.h"
#include "config.h"
#include "httplib.h"
#include "json.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

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
		nlohmann::json out;
		out["text"] = req.get_param_value("text");
		res.set_content(out.dump(), "application/json");
	});
	svr.Post("/json", [](const httplib::Request& req, httplib::Response& res) {
		try
		{
			nlohmann::json body = nlohmann::json::parse(req.body);
			body["ok"] = true;
			res.set_content(body.dump(), "application/json");
		}
		catch (const nlohmann::json::parse_error&)
		{
			res.status = 400;
		}
	});
	svr.Post("/bin", [](const httplib::Request& req, httplib::Response& res) {
		res.set_content(req.body.data(), req.body.size(), "application/octet-stream");
	});

	printf("web server listen %s:%d\n", ip.c_str(), port);
	fflush(stdout);
	return svr.listen(ip, port);
}
