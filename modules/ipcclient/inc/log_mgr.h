#pragma once
#include "rpc_coro.h"
#include <atomic>
#include <chrono>
#include <cstdint>

class LogMgr
{
	bool _on;
	int _tasks;
	int _line;
	int64_t _bytes;
	int _drain_sec;
	std::atomic<int64_t> _sent;
	std::atomic<int> _done;
	std::chrono::steady_clock::time_point _start;
public:
	LogMgr();
	static LogMgr& Get();
	void Load();
	void Start();
	coro::Task Pump();
};
