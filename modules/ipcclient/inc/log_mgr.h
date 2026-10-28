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
	bool _on;
	int32_t _tasks;
	int32_t _line;
	int64_t _bytes;
	int32_t _drain_sec;
	std::atomic<int64_t> _sent;
	std::atomic<int32_t> _done;
	std::chrono::steady_clock::time_point _start;
public:
	void Load();
	void Start();
	coro::Task Pump();
};
