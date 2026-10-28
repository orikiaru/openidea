#include "web_client.h"
#include "config.h"
#include "httplib.h"
#include "json.hpp"
#include <cstdio>
#include <cstring>
#include <string>

static bool OkStatus(const httplib::Result& res)
{
	return res && res->status == 200;
}

int WebClient::Run()
{
	std::string ip = "127.0.0.1";
	int port = 18080;
	TryGetConfig("WebClient.ip", ip);
	TryGetConfig("WebClient.port", port);

	httplib::Client cli(ip, port);
	cli.set_connection_timeout(2, 0);
	cli.set_read_timeout(5, 0);

	int bad = 0;

	const std::string text = "a b+";
	httplib::Result query = cli.Get("/query", httplib::Params{{"text", text}}, httplib::Headers{});
	std::string got_text;
	if (!OkStatus(query))
	{
		bad += 1;
	}
	else
	{
		try
		{
			nlohmann::json body = nlohmann::json::parse(query->body);
			got_text = body.at("text").get<std::string>();
			if (got_text != text)
			{
				bad += 1;
			}
		}
		catch (const nlohmann::json::exception&)
		{
			bad += 1;
		}
	}

	nlohmann::json req;
	req["n"] = 1;
	req["s"] = "hi";
	httplib::Result js = cli.Post("/json", req.dump(), "application/json");
	int got_n = 0;
	bool got_ok = false;
	std::string got_s;
	if (!OkStatus(js))
	{
		bad += 1;
	}
	else
	{
		try
		{
			nlohmann::json body = nlohmann::json::parse(js->body);
			got_n = body.at("n").get<int>();
			got_s = body.at("s").get<std::string>();
			got_ok = body.at("ok").get<bool>();
			if (got_n != 1 || got_s != "hi" || !got_ok)
			{
				bad += 1;
			}
		}
		catch (const nlohmann::json::exception&)
		{
			bad += 1;
		}
	}

	std::string bin("\x00\x01\xff", 3);
	httplib::Result raw = cli.Post("/bin", bin.data(), bin.size(), "application/octet-stream");
	int bin_ok = 0;
	if (OkStatus(raw) && raw->body.size() == bin.size() && std::memcmp(raw->body.data(), bin.data(), bin.size()) == 0)
	{
		bin_ok = 1;
	}
	else
	{
		bad += 1;
	}

	printf("web query=%d text=%s json_n=%d json_s=%s json_ok=%d bin=%d bad=%d\n",
		got_text == text ? 1 : 0, got_text.c_str(), got_n, got_s.c_str(), got_ok ? 1 : 0, bin_ok, bad);
	fflush(stdout);
	return bad == 0 ? 0 : 1;
}
