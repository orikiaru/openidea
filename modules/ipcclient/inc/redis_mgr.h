#pragma once
#include "rpc_coro.h"
#include "singleton.h"
#include <atomic>
#include <chrono>

class RedisMgr
{
	DECLARE_SINGLETON(RedisMgr)
	RedisMgr();
	bool _on;
	int _tasks;
	int _sync;
	int _seconds;
	int _timeout_ms;
	int _failed;
	std::atomic<int> _done;
	std::atomic<long long> _ops;
	std::atomic<int> _bad;
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
