#include "rpc_admission_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
#include "rpc_probe.h"
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


// 成功且标记对上返回 0，其他结果原样返回状态码。
OI::Coro::TaskResult<int32_t> RpcAdmissionMgr::AdmissionCall(DeferredRpcProbe* pClient)
{
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 1000;
	stRequest.iMark = 71;
	auto stResult = co_await OI::Rpc::RpcCall(*pClient, stRequest, OI::Timer::Seconds(5));
	if (stResult.GetRet() == 0 && stResult.GetRes().iMark != 71)
	{
		co_return OI_ERR_FAIL;
	}
	co_return stResult.GetRet();
}

OI::Coro::TaskResult<int> RpcAdmissionMgr::CheckRpcAdmission()
{
	static DeferredRpcProbe vClients[3];
	int iBad = 0;
	for (auto& stClient : vClients)
	{
		if (!(co_await stClient.StartClient()))
		{
			co_return 1;
		}
	}
	for (int iRound = 0; iRound < 3; ++iRound)
	{
		std::vector<OI::Coro::TaskResult<int32_t>> vCalls;
		for (int iClient = 0; iClient < 3; ++iClient)
		{
			for (int iIndex = 0; iIndex < 2; ++iIndex)
			{
				vCalls.push_back(AdmissionCall(&vClients[iClient]));
			}
		}
		int iOk = 0, iBusy = 0, iFailed = 0;
		int vAccepted[3]{};
		for (size_t iCall = 0; iCall < vCalls.size(); ++iCall)
		{
			const int32_t iReturnCode = co_await vCalls[iCall];
			if (iReturnCode == 0)
			{
				++iOk;
				++vAccepted[iCall / 2];
			}
			else if (iReturnCode == OI_ERR_BUSY)
			{
				++iBusy;
			}
			else
			{
				++iFailed;
			}
		}
		if (iOk != 2 || iBusy != 4 || iFailed != 0)
		{
			++iBad;
		}
		for (const int iAccepted : vAccepted)
		{
			if (iAccepted > 1)
			{
				++iBad;
			}
		}
		LOG_TRACE("hold rpc admission round=%d ok=%d busy=%d bad=%d", iRound, iOk, iBusy, iBad);
		co_await OI::Coro::yield(50);
	}
	// 超大请求拒绝不占额度，错误回执也必须归还；随后正常请求仍可执行。
	bool bBytes = false;
	TryGetConfig("Regression.rpc_admission_bytes", bBytes);
	if (bBytes)
	{
		struct LargeRequest : OI::RpcData::HoldReq
		{
			OI::Base::BytesStream& serialize(OI::Base::BytesStream& stOutput) const override
			{
				HoldReq::serialize(stOutput);
				stOutput.WritePb(99, std::string(128, 'x'));
				return stOutput;
			}
		} stLarge;
		OI::RpcData::HoldReq& stBase = stLarge;
		OI::RpcData::HoldRes stOutput;
		if ((co_await OI::Rpc::MakeRpcCallAwaiter(vClients[0], stBase, stOutput, OI::Timer::Seconds(3))) != OI_ERR_BUSY)
		{
			++iBad;
		}
	}
	for (int iKind = 0; iKind < 4; ++iKind)
	{
		OI::RpcData::HoldReq stRequest;
		stRequest.iDelayMilliseconds = iKind == 0 ? -1 : 0;
		stRequest.iMark = iKind == 1 ? -2147483600 : (iKind == 2 ? -2147483599 : 71);
		auto stResult = co_await OI::Rpc::RpcCall(vClients[0], stRequest, OI::Timer::Seconds(3));
		const int iExpected = iKind == 0 ? 1001 : (iKind < 3 ? OI_ERR_FAIL : 0);
		if (stResult.GetRet() != iExpected)
		{
			++iBad;
		}
		co_await OI::Coro::yield(50);
	}
	for (auto& stClient : vClients)
	{
		stClient.CloseAllSessions();
	}
	LOG_TRACE("hold rpc admission done bad=%d", iBad);
	fflush(stdout);
	co_return iBad;
}

// 服务端在途条数或字节超限时回 BUSY，额度恢复后请求仍能完成。只在准入用例里跑。
OI::Coro::TaskResult<int> RpcAdmissionMgr::Run()
{
	co_return co_await CheckRpcAdmission();
}
