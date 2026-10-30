#pragma once
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <atomic>
#include <chrono>
#include <map>
#include "oi_lock.h"
#include <string>

class EchoMgr
{
	DECLARE_SINGLETON(EchoMgr)
	EchoMgr();
	bool _on;
	bool _stack;
	int32_t _total;
	int32_t _timeout_ms;
	sid_t _sid;
	std::chrono::steady_clock::time_point _start;
	std::atomic<int32_t> _done;
	std::atomic<int32_t> _ok;
	std::atomic<int32_t> _rpc_fail;
	::OI::Thread::Mutex _mu;
	int32_t _mismatch;
	int32_t _gap_differ;
	int32_t _sample_sent;
	int32_t _sample_recv;
	int32_t _sample_code;
	bool _have_sample;
	std::map<std::string, int32_t> _fields;
public:
	void Load();
	bool StackOn() const { return _stack; }
	coro::Task StackProbe();
	void ArmTimeout();
	void Start(sid_t sid);
	sid_t Sid() const { return _sid; }
	int32_t Done() const { return _done.load(); }
	int32_t Ok() const { return _ok.load(); }
	void Note(const char* name);
	void NoteGap();
	void OnResult(int n, const ::OI::RpcData::AllEchoReq& req, const ::OI::RpcData::AllEchoRes& res, int ret, bool same);
	coro::Task Pump(int index);
};
