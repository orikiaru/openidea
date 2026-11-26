#pragma once
#include "oi_coro.h"
#include "oi_lock.h"
#include "oi_singleton.h"
#include <chrono>
#include <cstdint>

class TestEmpty
{
	DECLARE_SINGLETON(TestEmpty)
	TestEmpty();
	bool m_bEnabled;
	int32_t m_iTotal;
	int32_t m_iInflight;
	int32_t m_iTimeoutMilliseconds;
	std::chrono::steady_clock::time_point m_stStart;
	::OI::Thread::Mutex m_stMutex;
	int32_t m_iDone;
	int32_t m_iOk;
	int32_t m_iRpcFail;
	void Note(int32_t iCode);
	static OI::Coro::Task SendOne(int32_t iTimeoutMilliseconds);

  public:
	void Load();
	void Start();
	void ArmTimeout();
	OI::Coro::Task Run();
};
