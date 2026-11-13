#include "backpressure_mgr.h"
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

// 常驻测试管理器，使用真实会话及固定发送额度，不绕过底层发送判定。
class DirectBudgetProbe : public DeferredRpcProbe
{
  public:
	DirectBudgetProbe()
	{
		iMaxSendQueueBytes = 256;
		SetPendingCallLimits(16, 4096);
	}

  protected:
	bool AutoReconnect() const override
	{
		return false;
	}
};

OI::Coro::TaskResult<int> BackpressureMgr::CheckDirectBackpressure()
{
	static DirectBudgetProbe stClient;
	int iBad = 0;
	OI::RpcData::AllEchoReq stLarge;
	stLarge.stData.sMessage.assign(512, 'B');
	OI::RpcData::AllEchoRes stOutput;
	stOutput.stData.sMessage = "unchanged";
	// 当前任务内调用立即启动，连接尚未开始，稳定进入建连前排队路径。
	auto stQueued = OI::Rpc::RpcCall(stClient, stLarge, stOutput, OI::Timer::Seconds(3));
	const bool bConnected = (co_await stClient.StartClient()) == OI_OK;
	const int32_t iQueuedRet = co_await stQueued;
	if (!bConnected || iQueuedRet != OI_ERR_BUSY || stOutput.stData.sMessage != "unchanged")
	{
		++iBad;
	}
	const sid_t iSid = stClient.ConnectedSid();
	const int32_t iDirectRet = co_await OI::Rpc::RpcCall(stClient, stLarge, stOutput, OI::Timer::Seconds(3));
	const int32_t iPinnedRet = co_await OI::Rpc::RpcCall(stClient, iSid, stLarge, stOutput, OI::Timer::Seconds(3));
	if (iDirectRet != OI_ERR_BUSY || iPinnedRet != OI_ERR_BUSY || stOutput.stData.sMessage != "unchanged" || stClient.ConnectedSid() != iSid)
	{
		++iBad;
	}
	OI::RpcData::HoldReq stSmall;
	OI::RpcData::HoldRes stSmallOutput;
	stSmall.iMark = 975;
	const int32_t iRecoveryRet = co_await OI::Rpc::RpcCall(stClient, stSmall, stSmallOutput, OI::Timer::Seconds(3));
	if (iRecoveryRet != OI_OK || stSmallOutput.iMark != 975)
	{
		++iBad;
	}
	stClient.CloseAllSessions();
	const int32_t iClosedRet = co_await OI::Rpc::RpcCall(stClient, iSid, stSmall, stSmallOutput, OI::Timer::Seconds(3));
	if (iClosedRet != OI_ERR_CONNECT)
	{
		++iBad;
	}
	LOG_TRACE("hold direct backpressure queued=%d direct=%d pinned=%d closed=%d bad=%d", iQueuedRet, iDirectRet, iPinnedRet, iClosedRet, iBad);
	co_return iBad;
}

// 建连前排队、已连接直发、固定 sid 超出发送额度都回 BUSY。小请求仍成功，关闭后固定 sid 回 CONNECT。
OI::Coro::TaskResult<int> BackpressureMgr::Run()
{
	co_return co_await CheckDirectBackpressure();
}
