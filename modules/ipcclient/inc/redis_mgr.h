#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <atomic>
#include <chrono>

class RedisMgr
{
	DECLARE_SINGLETON(RedisMgr)
	RedisMgr();
	bool m_bEnabled;
	int32_t m_iTaskCount;
	int32_t m_iSynchronous;
	int32_t m_iSeconds;
	int32_t m_iTimeoutMilliseconds;
	int32_t m_iFailed;
	std::atomic<int32_t> m_iDone;
	std::atomic<int64_t> m_iOperations;
	std::atomic<int32_t> m_iBad;
	std::chrono::steady_clock::time_point m_stStart;

  public:
	void Load();
	bool Enabled() const
	{
		return m_bEnabled;
	}
	void Start();
	void NoteDone();
	void SyncStress(int iIdParam);
	coro::Task Pump();
	coro::Task Check();
	coro::Task Stress(int iIdParam);
};
