#include "encode_mgr.h"
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


OI::Coro::TaskResult<int> EncodeMgr::CheckServerEncoding()
{
	int iBadLocal = 0;
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = -2147483600;
	OI::RpcData::HoldRes stResponse;
	stResponse.iMark = 91;
	int32_t iResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
	if (iResult != OI_ERR_FAIL || stResponse.iMark != 91)
	{
		++iBadLocal;
	}
	stRequest.iMark = -2147483599;
	iResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
	if (iResult != OI_ERR_FAIL || stResponse.iMark != 91)
	{
		++iBadLocal;
	}
	const std::string sOriginal("error\n\r\0\\%s", 11);
	const OI::Log::DiagnosticText stEscaped(sOriginal);
	const OI::Log::DiagnosticText stLong(std::string(5000, 'X'));
	const OI::Log::DiagnosticText stExact(std::string(4096, 'X'));
	if (std::string(stEscaped.GetText()) != "error\\x0a\\x0d\\x00\\x5c%s" || stEscaped.IsTruncated() ||
	    !stLong.IsTruncated() || std::strlen(stLong.GetText()) != 4096 || stExact.IsTruncated())
	{
		++iBadLocal;
	}
	stRequest.iMark = -2147483597;
	stResponse.iMark = 91;
	iResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
	if (iResult != OI_ERR_FAIL || stResponse.iMark != 91)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold client malformed response sid=%llu bad=%d", (unsigned long long)IpcClient::GetInstance().ConnectedSid(), iBadLocal);
	stRequest.iMark = 92;
	auto stRecovered = co_await IpcClient::GetInstance().CallAny<OI::RpcData::HoldRes>(stRequest, OI::Timer::Seconds(2));
	if (stRecovered.GetRet() != 0 || stRecovered.GetRes().iMark != 92)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold server encoding bad=%d", iBadLocal);
	co_return iBadLocal;
}

// 服务端编码失败和损坏的响应体回 FAIL，不覆盖已有输出。同时核诊断文本的转义和截断。
OI::Coro::TaskResult<int> EncodeMgr::Run()
{
	co_return co_await CheckServerEncoding();
}
