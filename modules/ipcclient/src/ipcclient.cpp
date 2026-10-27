#include "ipcclient.h"
#include "allechoreq"
#include "allechores"
#include "base_header.h"
#include "rpc_coro.h"
#include "rpc_ids.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

IpcClient IpcClient::inst;

static const int kTotal = 1000000;
static sid_t g_sid = 0;
static std::chrono::steady_clock::time_point g_start;
static std::atomic<int> g_done(0);
static std::atomic<int> g_ok(0);
static std::atomic<int> g_rpc_fail(0);
static std::mutex g_mu;
static int g_mismatch = 0;
static int g_gap_differ = 0;
static int g_sample_sent = 0;
static int g_sample_recv = 0;
static int g_sample_code = 0;
static bool g_have_sample = false;
static std::map<std::string, int> g_fields;

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

static void Note(const char* name)
{
	g_fields[name] += 1;
}

static bool DiffAllTypes(const AllTypesData& sent, const AllTypesData& recv)
{
	bool same = true;
	if (sent.c != recv.c) { Note("c"); same = false; }
	if (sent.s != recv.s) { Note("s"); same = false; }
	if (sent.i != recv.i) { Note("i"); same = false; }
	if (sent.l != recv.l) { Note("l"); same = false; }
	if (sent.i64 != recv.i64) { Note("i64"); same = false; }
	if (sent.f != recv.f) { Note("f"); same = false; }
	if (sent.d != recv.d) { Note("d"); same = false; }
	if (!SameBytes(sent.raw, recv.raw)) { Note("raw"); same = false; }
	if (sent.msg != recv.msg) { Note("msg"); same = false; }
	if (sent.nums != recv.nums) { Note("nums"); same = false; }
	if (sent.words != recv.words) { Note("words"); same = false; }
	if (!SameInner(sent.child, recv.child)) { Note("child"); same = false; }
	if (sent.children.size() != recv.children.size())
	{
		Note("children");
		same = false;
	}
	else
	{
		for (size_t i = 0; i < sent.children.size(); ++i)
		{
			if (!SameInner(sent.children[i], recv.children[i]))
			{
				Note("children");
				same = false;
				break;
			}
		}
	}
	if (sent.table != recv.table) { Note("table"); same = false; }
	if (sent.named.size() != recv.named.size())
	{
		Note("named");
		same = false;
	}
	else
	{
		for (std::map<std::string, Inner>::const_iterator it = sent.named.begin(); it != sent.named.end(); ++it)
		{
			std::map<std::string, Inner>::const_iterator jt = recv.named.find(it->first);
			if (jt == recv.named.end() || !SameInner(it->second, jt->second))
			{
				Note("named");
				same = false;
				break;
			}
		}
	}
	if (sent.gap != recv.gap)
	{
		Note("gap");
		g_gap_differ += 1;
		same = false;
	}
	return same;
}

static void Report()
{
	std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
	long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - g_start).count();
	if (ms < 1)
	{
		ms = 1;
	}
	int ok = g_ok.load();
	int mismatch = g_mismatch;
	int rpc_fail = g_rpc_fail.load();
	printf("ipc mode=%s total=%d ok=%d mismatch=%d rpc_fail=%d ms=%lld rate=%d/s\n",
		kMode, kTotal, ok, mismatch, rpc_fail, ms, (int)(kTotal * 1000LL / ms));
	printf("sample gap_sent=%d gap_recv=%d code=%d gap_differ=%d\n",
		g_sample_sent, g_sample_recv, g_sample_code, g_gap_differ);
	printf("fields");
	for (std::map<std::string, int>::iterator it = g_fields.begin(); it != g_fields.end(); ++it)
	{
		printf(" %s=%d", it->first.c_str(), it->second);
	}
	printf("\n");
	fflush(stdout);
	ThreadPool::GetInstance()->RequestStop((mismatch == 0 && rpc_fail == 0 && ok == kTotal) ? 0 : 1);
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
	int ret = co_await RpcCall(IpcClient::GetInstance(), g_sid, req, res, trace);
	if (trace)
	{
		printf("coro await end token=%llu tid=%s ret=%d code=%d\n",
			(unsigned long long)token, TidText().c_str(), ret, res.code);
		fflush(stdout);
	}
	{
		std::lock_guard<std::mutex> lock(g_mu);
		if (!g_have_sample && n == 0)
		{
			g_have_sample = true;
			g_sample_sent = req.data.gap;
			g_sample_recv = res.data.gap;
			g_sample_code = res.code;
		}
		if (ret != 0 || res.code != 1)
		{
			g_rpc_fail.fetch_add(1);
		}
		else if (DiffAllTypes(req.data, res.data))
		{
			g_ok.fetch_add(1);
		}
		else
		{
			g_mismatch += 1;
		}
	}
	if (g_done.fetch_add(1) + 1 == kTotal)
	{
		Report();
	}
}

IpcClient::~IpcClient()
{

}

bool IpcClient::Init()
{
	return SessionManager::Init();
}

const std::string IpcClient::GetIDString()
{
    return "IpcClient";
}


void IpcClient::OnClientConnect(Session* pSession)
{
	g_sid = pSession->GetSessionID();
	g_start = std::chrono::steady_clock::now();
	printf("ipc client connect sid=%d mode=%s total=%d\n", g_sid, kMode, kTotal);
	fflush(stdout);
	for (int n = 0; n < kTotal; ++n)
	{
		coro::StartTask([n](coro::Token token) {
			return RunOne(token, n);
		});
	}
	BaseTimerManager::GetInstance()->AddTimer([]() {
		printf("ipc timeout done=%d ok=%d\n", g_done.load(), g_ok.load());
		fflush(stdout);
		ThreadPool::GetInstance()->RequestStop(1);
	}, 180000);
}

void IpcClient::OnClientDisconnect(const Session* pSession)
{
	(void)pSession;
}

void IpcClient::OnAbortSession()
{

}
