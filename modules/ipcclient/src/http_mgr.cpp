#include "http_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "oi_http.h"
#include "oi_log_client.h"
#include "oi_log.h"
#include "oi_registry.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "oi_rpcdata_jsonall.h"
#include <limits>
#include "oi_config.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "oi_poll.h"
#include "oi_net_session.h"
#include "oi_timer_manager.h"
#include <uv.h>
#include "test_board.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

OI::Coro::TaskResult<int> HttpMgr::CheckHttpFraming(int32_t iHttpPort)
{
	int iBadLocal = 0;
	OI::Http::Client stHttp("127.0.0.1", iHttpPort, 1000);
	struct HttpCase
	{
		const char* pPath;
		int iStatus;
		int32_t iCode;
		const char* pBody;
	};
	const std::vector<HttpCase> vCases{
	    {"good", 200, OI_OK, "ok"},
	    {"interim", 200, OI_OK, "ok"},
	    {"fragmented", 200, OI_OK, "ok"},
	    {"204", 204, OI_OK, ""},
	    {"304", 304, OI_OK, ""},
	    {"chunk-good", 200, OI_OK, "ok"},
	    {"length-junk", 200, OI_ERR_PROTOCOL, ""},
	    {"length-overflow", 200, OI_ERR_PROTOCOL, ""},
	    {"length-conflict", 200, OI_ERR_PROTOCOL, ""},
	    {"te-cl", 200, OI_ERR_PROTOCOL, ""},
	    {"chunk-separator", 200, OI_ERR_PROTOCOL, ""},
	    {"chunk-junk", 200, OI_ERR_PROTOCOL, ""},
	    {"truncated", 200, OI_ERR_PROTOCOL, ""},
	    {"only-interim", 100, OI_ERR_PROTOCOL, ""},
	    {"tail", 200, OI_OK, "ok"},
	    {"good", 200, OI_OK, "ok"},
	    {"chunk-tail", 200, OI_OK, "ok"},
	    {"good", 200, OI_OK, "ok"},
	    {"empty-tail", 204, OI_OK, ""},
	    {"good", 200, OI_OK, "ok"},
	};
	for (const auto& stCase : vCases)
	{
		auto stResult = co_await stHttp.Get(std::string("/") + stCase.pPath, {});
		if (stResult.iCode != stCase.iCode || (stCase.iCode == OI_OK && (stResult.iStatus != stCase.iStatus || stResult.sBody != stCase.pBody)))
		{
			++iBadLocal;
			LOG_TRACE("http regression path=%s status=%d code=%d error=%s", stCase.pPath, stResult.iStatus, stResult.iCode, stResult.sError.c_str());
		}
	}
	LOG_TRACE("hold http framing bad=%d", iBadLocal);
	co_return iBadLocal;
}

// 不复用时每条请求都重新连。服务端看到同一条连接上的后续请求会回 reused。
OI::Coro::TaskResult<int> HttpMgr::CheckHttpNoReuse(int32_t iHttpPort)
{
	int iBadLocal = 0;
	OI::Http::Client stHttp("127.0.0.1", iHttpPort, 1000, false);
	for (int iIndex = 0; iIndex < 2; ++iIndex)
	{
		auto stResult = co_await stHttp.Get("/good", {});
		if (stResult.iCode != OI_OK || stResult.iStatus != 200 || stResult.sBody != "ok")
		{
			++iBadLocal;
			LOG_TRACE("http no reuse index=%d status=%d code=%d error=%s", iIndex, stResult.iStatus, stResult.iCode, stResult.sError.c_str());
		}
	}
	LOG_TRACE("hold http no reuse bad=%d", iBadLocal);
	co_return iBadLocal;
}

size_t HttpMgr::AllocatedBytes()
{
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 33)
	const auto stMemory = mallinfo2();
#else
	const auto stMemory = mallinfo();
#endif
	return static_cast<size_t>(stMemory.uordblks) + static_cast<size_t>(stMemory.hblkhd);
#else
	return 0;
#endif
}

// 长超时期间连续成功的大响应，不应把已完成的收包容量留到超时。
OI::Coro::TaskResult<int> HttpMgr::CheckHttpRelease(int32_t iHttpPort)
{
	int iBadLocal = 0;
	OI::Http::Client stLargeHttp("127.0.0.1", iHttpPort, 30000);
	const size_t iBefore = AllocatedBytes();
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		auto stResult = co_await stLargeHttp.Get("/large", {});
		if (stResult.iCode != OI_OK || stResult.sBody != std::string(1024 * 1024, 'x'))
		{
			++iBadLocal;
		}
	}
	const size_t iAfter = AllocatedBytes();
	if (iAfter > iBefore + 8 * 1024 * 1024)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold http release before=%zu after=%zu bad=%d", iBefore, iAfter, iBadLocal);
	co_return iBadLocal;
}

// 分帧、中间响应、204、连接不复用，以及大响应释放后的堆增量。调用方在端口大于 0 时才跑。
OI::Coro::TaskResult<int> HttpMgr::Run(int32_t iHttpPort)
{
	int iBad = 0;
	iBad += co_await CheckHttpFraming(iHttpPort);
	iBad += co_await CheckHttpNoReuse(iHttpPort);
	iBad += co_await CheckHttpRelease(iHttpPort);
	co_return iBad;
}
