#include "echo_mgr.h"
#include "config.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "test_board.h"
#include "base_timer_manager.h"
#include "threadpool.h"
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

#ifdef _SINGLE_THREAD_
static const char* kMode = "st";
#else
static const char* kMode = "mt";
#endif

static bool SameBytes(const Bytes& a, const Bytes& b)
{
	if (a.Size() != b.Size())
	{
		return false;
	}
	if (a.Size() == 0)
	{
		return true;
	}
	return memcmp(a.Data(), b.Data(), a.Size()) == 0;
}

static bool SameInner(const Inner& a, const Inner& b)
{
	return a.n == b.n && a.s == b.s;
}

static void FillAllTypes(AllTypesData& data, int n)
{
	data.c = (char)(n & 0x7f);
	data.s = (short)(1000 + n);
	data.i = n;
	data.l = 100000L + n;
	data.i64 = 10000000000LL + n;
	data.f = 1.5f + (float)n;
	data.d = 2.5 + n;
	data.raw.Clear();
	char raw[4];
	memcpy(raw, &n, 4);
	data.raw.Append(raw, 4);
	data.msg = "msg-" + std::to_string(n);
	data.nums.clear();
	data.nums.push_back(n);
	data.nums.push_back(n + 1);
	data.words.clear();
	data.words.push_back("w");
	data.words.push_back(std::to_string(n));
	data.child.n = n;
	data.child.s = "child";
	data.children.clear();
	data.children.push_back(data.child);
	Inner kin;
	kin.n = n + 7;
	kin.s = "kin";
	data.children.push_back(kin);
	data.table.clear();
	data.table[n] = "t";
	data.table[n + 1] = "u";
	data.named.clear();
	data.named["a"] = data.child;
	data.named["b"] = kin;
	data.gap = 20 + n;
}

static void PoisonAllTypes(AllTypesData& data)
{
	data.c = 0x5a;
	data.s = -1;
	data.i = -2;
	data.l = -3;
	data.i64 = -4;
	data.f = -5.0f;
	data.d = -6.0;
	data.raw.Clear();
	data.msg = "poison";
	data.nums.clear();
	data.words.clear();
	data.child.n = -7;
	data.child.s = "poison";
	data.children.clear();
	data.table.clear();
	data.named.clear();
	data.gap = -12345;
}

static bool DiffAllTypes(const AllTypesData& sent, const AllTypesData& recv)
{
	bool same = true;
	if (sent.c != recv.c) { EchoMgr::Get().Note("c"); same = false; }
	if (sent.s != recv.s) { EchoMgr::Get().Note("s"); same = false; }
	if (sent.i != recv.i) { EchoMgr::Get().Note("i"); same = false; }
	if (sent.l != recv.l) { EchoMgr::Get().Note("l"); same = false; }
	if (sent.i64 != recv.i64) { EchoMgr::Get().Note("i64"); same = false; }
	if (sent.f != recv.f) { EchoMgr::Get().Note("f"); same = false; }
	if (sent.d != recv.d) { EchoMgr::Get().Note("d"); same = false; }
	if (!SameBytes(sent.raw, recv.raw)) { EchoMgr::Get().Note("raw"); same = false; }
	if (sent.msg != recv.msg) { EchoMgr::Get().Note("msg"); same = false; }
	if (sent.nums != recv.nums) { EchoMgr::Get().Note("nums"); same = false; }
	if (sent.words != recv.words) { EchoMgr::Get().Note("words"); same = false; }
	if (!SameInner(sent.child, recv.child)) { EchoMgr::Get().Note("child"); same = false; }
	if (sent.children.size() != recv.children.size())
	{
		EchoMgr::Get().Note("children");
		same = false;
	}
	else
	{
		for (size_t i = 0; i < sent.children.size(); ++i)
		{
			if (!SameInner(sent.children[i], recv.children[i]))
			{
				EchoMgr::Get().Note("children");
				same = false;
				break;
			}
		}
	}
	if (sent.table != recv.table) { EchoMgr::Get().Note("table"); same = false; }
	if (sent.named.size() != recv.named.size())
	{
		EchoMgr::Get().Note("named");
		same = false;
	}
	else
	{
		for (std::map<std::string, Inner>::const_iterator it = sent.named.begin(); it != sent.named.end(); ++it)
		{
			std::map<std::string, Inner>::const_iterator jt = recv.named.find(it->first);
			if (jt == recv.named.end() || !SameInner(it->second, jt->second))
			{
				EchoMgr::Get().Note("named");
				same = false;
				break;
			}
		}
	}
	if (sent.gap != recv.gap)
	{
		EchoMgr::Get().Note("gap");
		*EchoMgr::Get().GapDiffer() += 1;
		same = false;
	}
	return same;
}

static std::string TidText()
{
	std::ostringstream os;
	os << std::this_thread::get_id();
	return os.str();
}

static coro::Task RunOne(coro::Token token, int n)
{
	AllEchoReq req;
	FillAllTypes(req.data, n);
	AllEchoRes res;
	PoisonAllTypes(res.data);
	res.code = -1;
	const bool trace = (n == 0);
	if (trace)
	{
		printf("coro await begin token=%llu tid=%s\n", (unsigned long long)token, TidText().c_str());
		fflush(stdout);
	}
	int ret = co_await RpcCall(IpcClient::GetInstance(), EchoMgr::Get().Sid(), req, res, trace);
	if (trace)
	{
		printf("coro await end token=%llu tid=%s ret=%d code=%d\n",
			(unsigned long long)token, TidText().c_str(), ret, res.code);
		fflush(stdout);
	}
	EchoMgr::Get().OnResult(n, req, res, ret, DiffAllTypes(req.data, res.data));
}

EchoMgr::EchoMgr()
	: _on(true), _total(1000000), _timeout_ms(180000), _sid(0),
	  _done(0), _ok(0), _rpc_fail(0), _mismatch(0), _gap_differ(0),
	  _sample_sent(0), _sample_recv(0), _sample_code(0), _have_sample(false)
{
}

EchoMgr& EchoMgr::Get()
{
	static EchoMgr inst;
	return inst;
}

void EchoMgr::Load()
{
	TryGetConfig("Test.echo", _on);
	TryGetConfig("TestEcho.total", _total);
	TryGetConfig("TestEcho.timeout_ms", _timeout_ms);
}

void EchoMgr::Note(const char* name)
{
	_fields[name] += 1;
}

void EchoMgr::ArmTimeout()
{
	BaseTimerManager::GetInstance()->AddTimer([]() {
		printf("ipc timeout done=%d ok=%d\n", EchoMgr::Get().Done(), EchoMgr::Get().Ok());
		fflush(stdout);
		ThreadPool::GetInstance()->RequestStop(1);
	}, _timeout_ms);
}

void EchoMgr::Start(sid_t sid)
{
	if (!_on || _total <= 0)
	{
		return;
	}
	_sid = sid;
	_start = std::chrono::steady_clock::now();
	ArmTimeout();
	printf("ipc client connect sid=%d mode=%s total=%d\n", _sid, kMode, _total);
	fflush(stdout);
	GetTestBoard().Arm();
	coro::StartTask([](coro::Token) {
		return EchoMgr::Get().Pump(0);
	});
}

coro::Task EchoMgr::Pump(int index)
{
	// StartTask 会把协程跑到第一次挂起。先让出，避免分发把自己嵌进同一条调用栈。
	co_await coro::YieldToPool{};
	if (index >= _total)
	{
		co_return;
	}
	const int batch = 1024;
	int end = index + batch;
	if (end > _total)
	{
		end = _total;
	}
	for (int n = index; n < end; ++n)
	{
		coro::StartTask([n](coro::Token token) {
			return RunOne(token, n);
		});
	}
#ifdef _SINGLE_THREAD_
	// 协程只在主线程跑，这条线程接着才会进 poll。这一批丢完就让出。
	if (end < _total)
	{
		co_await coro::yield(1);
		co_await Pump(end);
	}
#else
	// 多线程协程在工人上跑，不在 poll 线程。这一轮 loop 结束，另起一条协程继续。
	if (end < _total)
	{
		coro::StartTask([end](coro::Token) {
			return EchoMgr::Get().Pump(end);
		});
	}
#endif
	co_return;
}

void EchoMgr::OnResult(int n, const AllEchoReq& req, const AllEchoRes& res, int ret, bool same)
{
	std::lock_guard<std::mutex> lock(_mu);
	if (!_have_sample && n == 0)
	{
		_have_sample = true;
		_sample_sent = req.data.gap;
		_sample_recv = res.data.gap;
		_sample_code = res.code;
	}
	if (ret != 0 || res.code != 1)
	{
		_rpc_fail.fetch_add(1);
	}
	else if (same)
	{
		_ok.fetch_add(1);
	}
	else
	{
		_mismatch += 1;
	}
	if (_done.fetch_add(1) + 1 != _total)
	{
		return;
	}
	long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - _start).count();
	if (ms < 1)
	{
		ms = 1;
	}
	int ok = _ok.load();
	int rpc_fail = _rpc_fail.load();
	printf("ipc mode=%s total=%d ok=%d mismatch=%d rpc_fail=%d ms=%lld rate=%d/s\n",
		kMode, _total, ok, _mismatch, rpc_fail, ms, (int)(_total * 1000LL / ms));
	printf("sample gap_sent=%d gap_recv=%d code=%d gap_differ=%d\n",
		_sample_sent, _sample_recv, _sample_code, _gap_differ);
	printf("fields");
	for (std::map<std::string, int>::iterator it = _fields.begin(); it != _fields.end(); ++it)
	{
		printf(" %s=%d", it->first.c_str(), it->second);
	}
	printf("\n");
	fflush(stdout);
	GetTestBoard().Done((_mismatch == 0 && rpc_fail == 0 && ok == _total) ? 0 : 1);
}
