#include "log_mgr.h"
#include "oi_config.h"
#include "oi_log_client.h"
#include "test_board.h"
#include <cstdio>
#include <vector>

LogMgr::LogMgr()
	: _on(false), _tasks(8), _line(64 * 1024), _bytes(1024LL * 1024 * 1024),
	  _drain_sec(15), _sent(0), _done(0)
{
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
	TestBoard::GetInstance().Arm();
	for (int i = 0; i < _tasks; ++i)
	{
		coro::StartTask([](coro::Token) {
			return LogMgr::GetInstance().Pump();
		});
	}
}

coro::Task LogMgr::Pump()
{
	std::vector<char> payload((size_t)_line, 'L');
	if (!payload.empty())
	{
		payload.back() = '\n';
	}
	int64_t prev = _sent.fetch_add(_line);
	if (prev < _bytes)
	{
		if (!LogClient::GetInstance().SendLog(LOG_INFO, payload.data(), payload.size()))
		{
			_sent.fetch_sub(_line);
		}
		// 下一条不等这条写完。单线程先 poll 把缓冲发出去，再跑新挂上的。
		coro::StartTask([](coro::Token) {
			return LogMgr::GetInstance().Pump();
		});
		co_return;
	}
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
			// 单线程只有这一条线程。sleep 会把 poll 冻住，套接字里的日志发不出去。
			co_await coro::yield(_drain_sec * 1000);
		}
		TestBoard::GetInstance().Done(0);
	}
	co_return;
}
