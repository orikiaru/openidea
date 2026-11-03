#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <atomic>
#include <chrono>
#include <cstdint>

class LogMgr
{
	DECLARE_SINGLETON(LogMgr)
	LogMgr();
	bool m_bEnabled;
	int32_t m_iTaskCount;
	int32_t m_iLineBytes;
	int64_t m_iTargetBytes;
	int32_t m_iDrainSeconds;
	std::atomic<int64_t> m_iSent;
	std::atomic<int32_t> m_iDone;
	std::chrono::steady_clock::time_point m_stStart;

  public:
	void Load();
	void Start();
	coro::Task Pump();
};
