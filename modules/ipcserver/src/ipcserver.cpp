#include "ipcserver.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "oi_parser.h"
#include "oi_rpc_call.h"
#include "oi_rpc_coro.h"
#include "rpc_ids.h"
#include "oi_config.h"
#include "oi_http.h"
#include <chrono>
#include <stdio.h>
#include <string>

IpcServer::~IpcServer()
{

}

const std::string IpcServer::GetIDString()
{
    return "IpcServer";
}

bool IpcServer::Init()
{
	::OI::Rpc::HandleRpc<::OI::RpcData::AllEchoReq, ::OI::RpcData::AllEchoRes>([](coro::Token token, ::OI::Net::SessionManager* mgr, sid_t sid, ::OI::RpcData::AllEchoReq& req, ::OI::RpcData::AllEchoRes& res) -> coro::Task {
		(void)token;
		(void)mgr;
		(void)sid;
		res.data = req.data;
		res.code = 1;
		co_return;
	});
	::OI::Rpc::HandleRpc<::OI::RpcData::HoldReq, ::OI::RpcData::HoldRes>([](coro::Token token, ::OI::Net::SessionManager* mgr, sid_t sid, ::OI::RpcData::HoldReq& req, ::OI::RpcData::HoldRes& res) -> coro::Task {
		(void)token;
		(void)mgr;
		(void)sid;
		if (req.delay_ms > 0)
		{
			co_await coro::yield(req.delay_ms);
		}
		res.mark = req.mark;
		co_return;
	});
	return ::OI::Net::SessionManager::Init();
}

static std::string HtmlTitle(const std::string& html)
{
	size_t open = html.find("<title>");
	if (open == std::string::npos)
	{
		return "";
	}
	open += 7;
	size_t close = html.find("</title>", open);
	if (close == std::string::npos)
	{
		return "";
	}
	return html.substr(open, close - open);
}

static coro::Task HttpProbe(std::string host, int32_t port, std::string path, int32_t timeout_ms)
{
	::OI::Http::Client cli(host, port, timeout_ms);
	std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
	::OI::Http::Response res = co_await cli.Get(path, {});
	long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count();
	printf("http probe host=%s port=%d path=%s status=%d bytes=%zu ms=%lld fail=%d err=%s title=%s\n",
		host.c_str(), port, path.c_str(), res.status, res.body.size(), ms, res.fail ? 1 : 0,
		res.error.c_str(), HtmlTitle(res.body).c_str());
	fflush(stdout);
}

void IpcServer::StartHttpProbe()
{
	std::string host;
	int32_t port = 80;
	std::string path = "/";
	int32_t timeout_ms = 5000;
	TryGetConfig("HttpProbe.host", host);
	TryGetConfig("HttpProbe.port", port);
	TryGetConfig("HttpProbe.path", path);
	TryGetConfig("HttpProbe.timeout_ms", timeout_ms);
	if (host.empty())
	{
		return;
	}
	coro::StartTask([host, port, path, timeout_ms](coro::Token) {
		return HttpProbe(host, port, path, timeout_ms);
	});
}

void IpcServer::OnAddSession(::OI::Net::Session* pSession)
{
	printf("ipc server session %d\n", pSession->GetSessionID());
	fflush(stdout);
}

void IpcServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	(void)pSession;
}
