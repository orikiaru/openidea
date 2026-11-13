#include "budget_mgr.h"
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


// 发送预算必须跨会话/通道缓冲连续计数，真正写出才恢复额度。
int BudgetMgr::CheckSendBudget()
{
	int iBadLocal = 0;
	struct BufferedSession : OI::Net::NetSession
	{
		BufferedSession()
		    : NetSession(nullptr)
		{
		}
		void OnDataIn() override
		{
		}
		void OnDataOut() override
		{
		}
	} stBuffered;
	stBuffered.SetSendQueueLimit(8);
	OI::Base::Bytes stPayload("12345678", 8), stStaged;
	if (stBuffered.SendResult(stPayload) != OI_OK || stBuffered.SendResult(stPayload) != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	stBuffered.SwapOutBuff(stStaged);
	if (stBuffered.PendingSendBytes() != 8 || stBuffered.Send(stPayload))
	{
		++iBadLocal;
	}
	stBuffered.OnBytesSend(8);
	stStaged.Clear();
	if (!stBuffered.Send(stPayload))
	{
		++iBadLocal;
	}
	stBuffered.ActiveClose();
	if (!stBuffered.IsClosed() || stBuffered.SendResult(stPayload) != OI_ERR_CONNECT)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold send budget bad=%d", iBadLocal);
	return iBadLocal;
}

// 发送额度跨会话缓冲连续计数，真正写出才恢复。关闭后发送回 CONNECT。
int BudgetMgr::Run()
{
	return CheckSendBudget();
}
