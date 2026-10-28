#pragma once
#include "rpc_coro.h"

// 超时和回包撞车。和回声、日志、Redis 压测分开，关掉就不跑。
class HoldMgr
{
	bool _on;
	sid_t _sid;
	int _redis_timeout_ms;
public:
	HoldMgr();
	static HoldMgr& Get();
	void Load();
	bool Enabled() const { return _on; }
	void Start(sid_t sid);
	coro::Task Pump();
};
