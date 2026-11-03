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

static OI_RPC_SYNC_HANDLER(AllEcho, HandleAllEcho)
{
	stResponse.stData = stRequest.stData;
	stResponse.iCode = 1;
	return 0;
}

static OI_RPC_HANDLER(Hold, HandleHold)
{
	if (stRequest.iDelayMilliseconds < 0)
	{
		co_return 1001; // 业务错误：延迟不能为负，回执不带响应体。
	}
	if (stRequest.iDelayMilliseconds > 0)
	{
		co_await coro::yield(stRequest.iDelayMilliseconds);
	}
	stResponse.iMark = stRequest.iMark;
	co_return 0;
}

bool IpcServer::Init()
{
	OI_RPC_BIND(AllEcho, HandleAllEcho);
	OI_RPC_BIND(Hold, HandleHold);
	return ::OI::Net::SessionManager::Init();
}

static std::string HtmlTitle(const std::string& sHtmlParam)
{
	size_t iOpen = sHtmlParam.find("<title>");
	if (iOpen == std::string::npos)
	{
		return "";
	}
	iOpen += 7;
	size_t iClose = sHtmlParam.find("</title>", iOpen);
	if (iClose == std::string::npos)
	{
		return "";
	}
	return sHtmlParam.substr(iOpen, iClose - iOpen);
}

static coro::Task HttpProbe(std::string sHostParam, int32_t iPortParam, std::string sPathParam, int32_t iTimeoutMillisecondsParam)
{
	::OI::Http::Client stCli(sHostParam, iPortParam, iTimeoutMillisecondsParam);
	std::chrono::steady_clock::time_point stBegin = std::chrono::steady_clock::now();
	::OI::Http::Response stResponseLocal = co_await stCli.Get(sPathParam, {});
	long long iMillisecondsLocal = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stBegin).count();
	printf("http probe host=%s port=%d path=%s status=%d bytes=%zu ms=%lld fail=%d err=%s title=%s\n",
		sHostParam.c_str(), iPortParam, sPathParam.c_str(), stResponseLocal.iStatus, stResponseLocal.sBody.size(), iMillisecondsLocal, stResponseLocal.bFail ? 1 : 0,
		stResponseLocal.sError.c_str(), HtmlTitle(stResponseLocal.sBody).c_str());
	fflush(stdout);
}

void IpcServer::StartHttpProbe()
{
	std::string sHostLocal;
	int32_t iPortLocal = 80;
	std::string sPath = "/";
	int32_t iTimeoutMillisecondsLocal = 5000;
	TryGetConfig("HttpProbe.host", sHostLocal);
	TryGetConfig("HttpProbe.port", iPortLocal);
	TryGetConfig("HttpProbe.path", sPath);
	TryGetConfig("HttpProbe.timeout_ms", iTimeoutMillisecondsLocal);
	if (sHostLocal.empty())
	{
		return;
	}
	coro::StartTask([sHostLocal, iPortLocal, sPath, iTimeoutMillisecondsLocal](coro::Token) {
		return HttpProbe(sHostLocal, iPortLocal, sPath, iTimeoutMillisecondsLocal);
	});
}

void IpcServer::OnAddSession(::OI::Net::Session* pSessionParam)
{
	printf("ipc server session %d\n", pSessionParam->GetSessionID());
	fflush(stdout);
}

void IpcServer::OnDelSession(const ::OI::Net::Session* pSessionParam)
{
	(void)pSessionParam;
}
