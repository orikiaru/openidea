#pragma once
#include "rpc_coro.h"
#include "singleton.h"
#include <atomic>
#include <chrono>
#include <cstdint>

class LogMgr
{
	DECLARE_SINGLETON(LogMgr)
	LogMgr();
	bool _on;
	int _tasks;
	int _line;
	int64_t _bytes;
	int _drain_sec;
	std::atomic<int64_t> _sent;
	std::atomic<int> _done;
	std::chrono::steady_clock::time_point _start;
public:
	void Load();
	void Start();
	coro::Task Pump();
};
