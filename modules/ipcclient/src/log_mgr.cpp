#include "log_mgr.h"
#include "config.h"
#include "log_client.h"
#include "test_board.h"
#include <cstdio>
#include <thread>
#include <vector>

LogMgr::LogMgr()
	: _on(false), _tasks(8), _line(64 * 1024), _bytes(1024LL * 1024 * 1024),
	  _drain_sec(15), _sent(0), _done(0)
{
}

LogMgr& LogMgr::Get()
{
	static LogMgr inst;
	return inst;
}

void LogMgr::Load()
{
	TryGetConfig("Test.log", _on);
	TryGetConfig("TestLog.tasks", _tasks);
	TryGetConfig("TestLog.line", _line);
	TryGetConfig("TestLog.bytes", _bytes);
	TryGetConfig("TestLog.drain_sec", _drain_sec);
}

void LogMgr::Start()
{
	if (!_on || _tasks <= 0 || _line <= 0 || _bytes <= 0)
	{
		return;
	}
	_start = std::chrono::steady_clock::now();
	GetTestBoard().Arm();
	for (int i = 0; i < _tasks; ++i)
	{
		coro::StartTask([](coro::Token) {
			return LogMgr::Get().Pump();
		});
	}
}

coro::Task LogMgr::Pump()
{
	co_await coro::YieldToPool{};
	std::vector<char> payload((size_t)_line, 'L');
	if (!payload.empty())
	{
		payload.back() = '\n';
	}
#ifdef _SINGLE_THREAD_
	// 和 poll 同一条主线程。让出后队列空了，主循环才能进 poll，把日志写进套接字。
	for (;;)
	{
		int64_t prev = _sent.fetch_add(_line);
		if (prev >= _bytes)
		{
			break;
		}
		if (!LogClient::GetInstance().SendLog(LOG_INFO, payload.data(), payload.size()))
		{
			_sent.fetch_sub(_line);
		}
		co_await coro::yield(1);
	}
#else
	int64_t prev = _sent.fetch_add(_line);
	if (prev < _bytes)
	{
		if (!LogClient::GetInstance().SendLog(LOG_INFO, payload.data(), payload.size()))
		{
			_sent.fetch_sub(_line);
		}
		// 这一轮结束。下一条日志另起一个协程。
		coro::StartTask([](coro::Token) {
			return LogMgr::Get().Pump();
		});
		co_return;
	}
#endif
	if (_done.fetch_add(1) + 1 == _tasks)
	{
		long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - _start).count();
		if (ms < 1)
		{
			ms = 1;
		}
		long long sent = _sent.load();
		printf("log tasks=%d sent=%lld ms=%lld enqueue_rate=%lldMB/s\n",
			_tasks, sent, ms, sent / ms * 1000 / (1024 * 1024));
		fflush(stdout);
		if (_drain_sec > 0)
		{
			std::this_thread::sleep_for(std::chrono::seconds(_drain_sec));
		}
		GetTestBoard().Done(0);
	}
	co_return;
}
