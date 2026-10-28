#include "web_client.h"
#include "oi_config.h"
#include "oi_http.h"
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

static bool OkStatus(const ::OI::Http::Response& res)
{
	return !res.fail && res.status == 200;
}

static coro::Task Run()
{
	std::string ip = "127.0.0.1";
	int32_t port = 18080;
	TryGetConfig("WebClient.ip", ip);
	TryGetConfig("WebClient.port", port);

	::OI::Http::Client cli(ip, port);
	int bad = 0;

	const std::string text = "a b+";
	std::vector<std::pair<std::string, std::string>> query;
	query.push_back(std::make_pair(std::string("text"), text));
	::OI::Http::Response got = co_await cli.Get("/query", query);
	std::string got_text;
	if (!OkStatus(got))
	{
		bad += 1;
	}
	else
	{
		Json::Value body;
		if (!ParseJson(got.body, body) || !body.isMember("text") || !body["text"].isString())
		{
			bad += 1;
		}
		else
		{
			got_text = body["text"].asString();
			if (got_text != text)
			{
				bad += 1;
			}
		}
	}

	Json::Value req(Json::objectValue);
	req["n"] = 1;
	req["s"] = "hi";
	::OI::Http::Response js = co_await cli.Post("/json", DumpJson(req), "application/json");
	int got_n = 0;
	bool got_ok = false;
	std::string got_s;
	if (!OkStatus(js))
	{
		bad += 1;
	}
	else
	{
		Json::Value body;
		if (!ParseJson(js.body, body) || !body.isMember("n") || !body.isMember("s") || !body.isMember("ok"))
		{
			bad += 1;
		}
		else
		{
			got_n = body["n"].asInt();
			got_s = body["s"].asString();
			got_ok = body["ok"].asBool();
			if (got_n != 1 || got_s != "hi" || !got_ok)
			{
				bad += 1;
			}
		}
	}

	::OI::RpcData::JsonAll sent;
	sent.c = 7;
	sent.s = 1001;
	sent.i = 42;
	sent.l = 100000;
	sent.i64 = 10000000000LL;
	sent.f = 1.5f;
	sent.d = 2.5;
	sent.raw.Append("raw", 3);
	sent.msg = "msg";
	sent.nums.push_back(1);
	sent.nums.push_back(2);
	sent.words.push_back("w");
	sent.words.push_back("ord");
	sent.child.n = 3;
	sent.child.s = "child";
	::OI::RpcData::Inner kin;
	kin.n = 9;
	kin.s = "kin";
	sent.children.push_back(sent.child);
	sent.children.push_back(kin);
	sent.table[1] = "t";
	sent.table[2] = "u";
	sent.named["a"] = sent.child;
	sent.named["b"] = kin;
	sent.u8 = 200;
	sent.u16 = 40000;
	sent.u32 = 3000000000u;
	sent.u64 = (1ULL << 63) + 9;
	sent.unums.push_back(sent.u64);
	sent.unums.push_back(1);
	sent.gap = 20;
	sent.utable[sent.u32] = "u";
	sent.ubig[sent.u64] = "b";
	::OI::Http::Response rpc = co_await cli.Post("/rpcjson", sent.ToJson(), "application/json");
	int rpc_ok = 0;
	if (!OkStatus(rpc))
	{
		bad += 1;
	}
	else
	{
		::OI::RpcData::JsonAll back;
		bool same = back.FromJson(rpc.body)
			&& back.c == sent.c && back.s == sent.s && back.i == sent.i
			&& back.l == sent.l && back.i64 == sent.i64
			&& back.f == sent.f && back.d == sent.d
			&& back.raw.Size() == sent.raw.Size()
			&& (sent.raw.Size() == 0 || std::memcmp(back.raw.Data(), sent.raw.Data(), sent.raw.Size()) == 0)
			&& back.msg == sent.msg && back.nums == sent.nums && back.words == sent.words
			&& back.child.n == sent.child.n && back.child.s == sent.child.s
			&& back.children.size() == sent.children.size()
			&& back.table == sent.table && back.named.size() == sent.named.size()
			&& back.u8 == sent.u8 && back.u16 == sent.u16 && back.u32 == sent.u32 && back.u64 == sent.u64
			&& back.unums == sent.unums && back.gap == sent.gap + 1
			&& back.utable == sent.utable && back.ubig == sent.ubig;
		if (same)
		{
			for (size_t i = 0; i < sent.children.size(); ++i)
			{
				if (back.children[i].n != sent.children[i].n || back.children[i].s != sent.children[i].s)
				{
					same = false;
					break;
				}
			}
		}
		if (same)
		{
			for (std::map<std::string, ::OI::RpcData::Inner>::const_iterator it = sent.named.begin(); it != sent.named.end(); ++it)
			{
				std::map<std::string, ::OI::RpcData::Inner>::const_iterator jt = back.named.find(it->first);
				if (jt == back.named.end() || jt->second.n != it->second.n || jt->second.s != it->second.s)
				{
					same = false;
					break;
				}
			}
		}
		if (same)
		{
			rpc_ok = 1;
		}
		else
		{
			bad += 1;
		}
	}

	std::string bin("\x00\x01\xff", 3);
	::OI::Http::Response raw = co_await cli.Post("/bin", bin, "application/octet-stream");
	int bin_ok = 0;
	if (OkStatus(raw) && raw.body.size() == bin.size() && std::memcmp(raw.body.data(), bin.data(), bin.size()) == 0)
	{
		bin_ok = 1;
	}
	else
	{
		bad += 1;
	}

	printf("web query=%d text=%s json_n=%d json_s=%s json_ok=%d rpc=%d bin=%d bad=%d\n",
		got_text == text ? 1 : 0, got_text.c_str(), got_n, got_s.c_str(), got_ok ? 1 : 0, rpc_ok, bin_ok, bad);
	if (bad != 0)
	{
		printf("web err query=%s json=%s rpc=%s bin=%s\n",
			got.error.c_str(), js.error.c_str(), rpc.error.c_str(), raw.error.c_str());
	}
	fflush(stdout);
	::OI::Thread::ThreadPool::GetInstance().RequestStop(bad == 0 ? 0 : 1);
}

void WebClient::Start()
{
	coro::StartTask([](coro::Token) {
		return Run();
	});
}
