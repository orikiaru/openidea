#pragma once
#include "allechoreq"
#include "allechores"
#include "rpc_coro.h"
#include "singleton.h"
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <string>

class EchoMgr
{
	DECLARE_SINGLETON(EchoMgr)
	EchoMgr();
	bool _on;
	int _total;
	int _timeout_ms;
	sid_t _sid;
	std::chrono::steady_clock::time_point _start;
	std::atomic<int> _done;
	std::atomic<int> _ok;
	std::atomic<int> _rpc_fail;
	std::mutex _mu;
	int _mismatch;
	int _gap_differ;
	int _sample_sent;
	int _sample_recv;
	int _sample_code;
	bool _have_sample;
	std::map<std::string, int> _fields;
public:
	void Load();
	void ArmTimeout();
	void Start(sid_t sid);
	sid_t Sid() const { return _sid; }
	int Done() const { return _done.load(); }
	int Ok() const { return _ok.load(); }
	void Note(const char* name);
	int* GapDiffer() { return &_gap_differ; }
	void OnResult(int n, const OI::Rpc::AllEchoReq& req, const OI::Rpc::AllEchoRes& res, int ret, bool same);
	coro::Task Pump(int index);
};
