#include "empty_mgr.h"
#include "ipcclient.h"
#include "oi_config.h"
#include "oi_errno.h"
#include "oi_log.h"
#include "oi_rpc_coro.h"
#include "rpc_ids.h"
#include "oi_rpcdata_emptyreq.h"
#include "oi_rpcdata_emptyres.h"
#include "oi_thread.h"
#include "oi_timer_manager.h"
#include "test_board.h"
#include <cstdio>

#ifdef _SINGLE_THREAD_
static const char* kMode = "st";
#else
static const char* kMode = "mt";
#endif

EmptyMgr::EmptyMgr()
    : m_bEnabled(false), m_iTotal(1000000), m_iInflight(2048), m_iTimeoutMilliseconds(180000), m_iDone(0), m_iOk(0), m_iRpcFail(0)
{
}

void EmptyMgr::Note(int32_t iCode)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	if (iCode == OI_OK)
	{
		++m_iOk;
	}
	else
	{
		++m_iRpcFail;
	}
	++m_iDone;
}

void EmptyMgr::Load()
{
	TryGetConfig("Test.empty", m_bEnabled);
	TryGetConfig("TestEmpty.total", m_iTotal);
	TryGetConfig("TestEmpty.inflight", m_iInflight);
	if (m_iInflight < 1)
	{
		m_iInflight = 1;
	}
	TryGetConfig("TestEmpty.timeout_ms", m_iTimeoutMilliseconds);
}

void EmptyMgr::ArmTimeout()
{
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([]()
	{
		LOG_TRACE("empty timeout");
		fflush(stdout);
		::OI::Thread::ThreadPool::GetInstance().RequestStop(1);
	}, m_iTimeoutMilliseconds);
}

void EmptyMgr::Start()
{
	if (!m_bEnabled || m_iTotal <= 0)
	{
		return;
	}
	m_stStart = std::chrono::steady_clock::now();
	ArmTimeout();
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return EmptyMgr::GetInstance().Run();
	});
}

OI::Coro::Task EmptyMgr::SendOne(int32_t iTimeoutMilliseconds)
{
	int32_t iCode = OI_ERR_FAIL;
	try
	{
		::OI::RpcData::EmptyReq stRequest;
		const auto stResult = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), std::move(stRequest), ::OI::Timer::Milliseconds(iTimeoutMilliseconds));
		iCode = stResult.GetRet();
	}
	catch (const std::exception&)
	{
		iCode = OI_ERR_FAIL;
	}
	EmptyMgr::GetInstance().Note(iCode);
}

OI::Coro::Task EmptyMgr::Run()
{
	const int32_t iTimeoutMilliseconds = m_iTimeoutMilliseconds;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		m_iDone = 0;
		m_iOk = 0;
		m_iRpcFail = 0;
	}
	// 一条协议一个 StartTask。在途数到 inflight 就停，每 1 毫秒让出后再补。
	int32_t iStarted = 0;
	while (iStarted < m_iTotal)
	{
		int32_t iDone = 0;
		{
			::OI::Thread::MutexGuard stLock(m_stMutex);
			iDone = m_iDone;
		}
		if (iStarted - iDone >= m_iInflight)
		{
			co_await OI::Coro::yield(1);
			continue;
		}
		OI::Coro::StartTask([iTimeoutMilliseconds](OI::Coro::Token)
		{
			return EmptyMgr::SendOne(iTimeoutMilliseconds);
		});
		++iStarted;
	}
	for (;;)
	{
		int32_t iDone = 0;
		{
			::OI::Thread::MutexGuard stLock(m_stMutex);
			iDone = m_iDone;
		}
		if (iDone >= m_iTotal)
		{
			break;
		}
		co_await OI::Coro::yield(1);
	}
	int32_t iOk = 0;
	int32_t iRpcFail = 0;
	{
		::OI::Thread::MutexGuard stLock(m_stMutex);
		iOk = m_iOk;
		iRpcFail = m_iRpcFail;
	}
	long long iMillisecondsLocal = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - m_stStart)
	                                   .count();
	if (iMillisecondsLocal < 1)
	{
		iMillisecondsLocal = 1;
	}
	const long long iRate = m_iTotal * 1000LL / iMillisecondsLocal;
	LOG_TRACE("empty mode=%s total=%d ms=%lld rate=%lld/s rpc_fail=%d",
	    kMode, m_iTotal, iMillisecondsLocal, iRate, iRpcFail);
	fflush(stdout);
	TestBoard::GetInstance().Done((iRpcFail == 0 && iOk == m_iTotal) ? 0 : 1);
}
