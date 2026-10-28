#pragma once
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <atomic>
#include <chrono>

class RedisMgr
{
	DECLARE_SINGLETON(RedisMgr)
	RedisMgr();
	bool _on;
	int32_t _tasks;
	int32_t _sync;
	int32_t _seconds;
	int32_t _timeout_ms;
	int32_t _failed;
	std::atomic<int32_t> _done;
	std::atomic<int64_t> _ops;
	std::atomic<int32_t> _bad;
	std::chrono::steady_clock::time_point _start;
public:
	void Load();
	bool Enabled() const { return _on; }
	void Start();
	void NoteDone();
	void SyncStress(int id);
	coro::Task Pump();
	coro::Task Check();
	coro::Task Stress(int id);
};
