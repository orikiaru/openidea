#include "hold_mgr.h"
#include "oi_config.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "oi_poll.h"
#include "test_board.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>

HoldMgr::HoldMgr()
	: _on(false), _sid(0), _redis_timeout_ms(25000)
{
}

void HoldMgr::Load()
{
	TryGetConfig("Test.hold", _on);
	TryGetConfig("RedisClient.timeout_ms", _redis_timeout_ms);
}

void HoldMgr::Start(sid_t sid)
{
	if (!_on)
	{
		return;
	}
	_sid = sid;
	TestBoard::GetInstance().Arm();
	coro::StartTask([](coro::Token) {
		return HoldMgr::GetInstance().Pump();
	});
}

static long long CpuMs(std::clock_t begin)
{
	double ms = 1000.0 * (double)(std::clock() - begin) / (double)CLOCKS_PER_SEC;
	if (ms < 0)
	{
		ms = 0;
	}
	return (long long)ms;
}

static long long WallMs(std::chrono::steady_clock::time_point begin)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now() - begin).count();
}

static bool Pong(const ::OI::Redis::RedisResult& result)
{
	return !result.timeout && result.reply && result.reply->type == REDIS_REPLY_STATUS
		&& result.reply->str && std::strcmp(result.reply->str, "PONG") == 0;
}

struct RaceBox
{
	int32_t rets[8];
	int32_t marks[8];
	std::atomic<int32_t> finished;
	RaceBox() : finished(0)
	{
		for (int i = 0; i < 8; ++i)
		{
			rets[i] = -99;
			marks[i] = 0;
		}
	}
};

static coro::Task RaceOne(sid_t sid, int32_t index, int32_t mark, int32_t delay, std::shared_ptr<RaceBox> box)
{
	::OI::RpcData::HoldReq one;
	::OI::RpcData::HoldRes back;
	one.delay_ms = delay;
	one.mark = mark;
	int code = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), sid, one, back, 10);
	box->rets[index] = code;
	box->marks[index] = back.mark;
	box->finished.fetch_add(1, std::memory_order_release);
	co_return;
}

coro::Task HoldMgr::Pump()
{
	int bad = 0;
	// 发送预算必须跨会话/通道缓冲连续计数，真正写出才恢复额度。
	struct BufferedSession : OI::Net::NetSession
	{
		BufferedSession() : NetSession(nullptr) {}
		void OnDataIn() override {}
		void OnDataOut() override {}
	} buffered;
	buffered.SetSendQueueLimit(8);
	OI::Base::Bytes payload("12345678", 8), staged;
	if (!buffered.Send(payload) || buffered.Send(payload)) ++bad;
	buffered.SwapOutBuff(staged);
	if (buffered.PendingSendBytes() != 8 || buffered.Send(payload)) ++bad;
	buffered.OnBytesSent(8);
	staged.Clear();
	if (!buffered.Send(payload)) ++bad;
	buffered.ActiveClose();
	if (!buffered.IsClosed() || buffered.Send(payload)) ++bad;
	printf("hold send budget bad=%d\n", bad);

	// 持续就绪的协程不能饿死同优先级普通任务。
	std::atomic<bool> ordinary_done{false};
	coro::TaskScope busy_scope;
	const auto fairness_begin = std::chrono::steady_clock::now();
	for (int i = 0; i < 64; ++i)
		busy_scope.Start([&ordinary_done, fairness_begin](coro::Token, coro::StopToken stop) -> coro::Task {
			while (!ordinary_done.load() && !stop.StopRequested()
				&& std::chrono::steady_clock::now() - fairness_begin < OI::Timer::Seconds(3))
				co_await coro::yield(0);
		});
	co_await coro::yield(OI::Timer::Milliseconds(50));
	if (!OI::Thread::ThreadPool::GetInstance().AddTask([&ordinary_done]() { ordinary_done.store(true); })) { ++bad; ordinary_done.store(true); }
	while (!ordinary_done.load()) co_await coro::yield(0);
	if (WallMs(fairness_begin) > 1500) ++bad;
	busy_scope.RequestStop();
	co_await busy_scope.Join();
	printf("hold scheduler fairness bad=%d\n", bad);

	// 子任务异常跨挂起点传回父任务；作用域关闭后不再接受任务。
	auto throwing = [](coro::Token, coro::StopToken) -> coro::Task {
		co_await coro::yield(::OI::Timer::Milliseconds(10));
		throw std::runtime_error("scope regression");
	};
	coro::TaskScope failures;
	failures.Start(throwing);
	bool caught = false;
	try { co_await failures.Join(); }
	catch (const std::runtime_error&) { caught = true; }
	if (!caught) ++bad;
	coro::TaskScope scope;
	std::atomic<int> finished{0}, started{0};
	for (int i = 0; i < 128; ++i)
		scope.Start([&finished, &started](coro::Token, coro::StopToken stop) -> coro::Task {
			++started;
			while (!stop.StopRequested()) co_await coro::yield(::OI::Timer::Milliseconds(10));
			++finished;
		});
	while (started.load() != 128) co_await coro::yield(::OI::Timer::Milliseconds(10));
	scope.RequestStop();
	co_await scope.Join();
	if (finished.load() != 128) ++bad;
	try { scope.Start(throwing); ++bad; } catch (const std::logic_error&) {}
	printf("hold scope finished=%d caught=%d bad=%d\n", finished.load(), caught, bad);
	// 真正的带捕获协程 lambda：临时闭包离开后、挂起恢复后仍访问捕获。
	struct Captures { std::atomic<int> done{0}; std::atomic<int> bad{0}; };
	auto captures = std::make_shared<Captures>();
	for (int i = 0; i < 32; ++i)
	{
		coro::StartTask([captures, text = std::string(4096, 'a' + i % 26), i](coro::Token token) -> coro::Task {
			co_await coro::yield(::OI::Timer::Milliseconds(0));
			co_await coro::yield(::OI::Timer::Milliseconds(10));
			if (!token || text != std::string(4096, 'a' + i % 26)) captures->bad.fetch_add(1);
			captures->done.fetch_add(1, std::memory_order_release);
		});
	}
	while (captures->done.load(std::memory_order_acquire) < 32) co_await coro::yield(::OI::Timer::Milliseconds(10));
	bad += captures->bad.load();
	printf("hold captured tasks done=%d bad=%d\n", captures->done.load(), captures->bad.load());

	// 使用真实待连接队列验证条数、字节限制以及取消后的容量恢复。
	struct PendingClient : OI::Rpc::ProtoClientManager
	{
		int failure_mode = 0;
		void SetSendLimit(size_t bytes) { _max_send_queue_bytes = bytes; }
		OI::Rpc::ProtoSession* AttachForTest()
		{
			auto session = new OI::Rpc::ProtoSession(this);
			AddSession(session);
			return session;
		}
		void OnClientConnect(OI::Net::Session*) override {}
		void OnClientDisconnect(const OI::Net::Session*) override {}
		bool TryPendCall(uint64_t id, const OI::Base::Bytes& data) override
		{
			if (!failure_mode) return ProtoClientManager::TryPendCall(id, data);
			if (failure_mode == 2) OI::Rpc::WakeRpcCall(id, OI_ERR_FAIL, nullptr);
			if (failure_mode == 3) ProtoClientManager::TryPendCall(id, data);
			if (failure_mode == 4)
				coro::StartTask([id](coro::Token) -> coro::Task {
					OI::Rpc::WakeRpcCall(id, OI_ERR_FAIL, nullptr);
					co_return;
				});
			throw std::runtime_error("injected submission failure");
		}
	} pending;
	{
		PendingClient raw_pending;
		raw_pending.SetSendLimit(8);
		if (!raw_pending.SendRawOrPend(payload, 1024) || raw_pending.SendRawOrPend(payload, 1024)) ++bad;
		auto session = raw_pending.AttachForTest();
		if (session->PendingSendBytes() != 8 || raw_pending.SendRaw(payload)) ++bad;
		raw_pending.CloseSession(session->GetSessionID());
		session->Release();
	}
	printf("hold preconnect send budget bad=%d\n", bad);
	pending.SetPendingCallLimits(1, 8);
	OI::Base::BytesStream frame;
	frame << uint64_t(1);
	int first = 0, second = 0;
	auto bind = [&](int& result) {
		return OI::Rpc::BindRpcCall(&pending, INVALID_SID, 0, {}, &result, 0, {}, []() {});
	};
	auto a = bind(first);
	pending.TryPendCall(a, frame.GetMutableBuff());
	auto b = bind(second);
	pending.TryPendCall(b, frame.GetMutableBuff());
	if (second != OI_ERR_BUSY) ++bad;
	OI::Rpc::WakeRpcCall(a, OI_ERR_TIMEOUT, nullptr);
	second = 0;
	b = bind(second);
	pending.TryPendCall(b, frame.GetMutableBuff());
	if (second != 0) ++bad;
	pending.FailHeldCalls();
	if (second != OI_ERR_CONNECT) ++bad;
	pending.SetPendingCallLimits(8, 7);
	second = 0;
	b = bind(second);
	pending.TryPendCall(b, frame.GetMutableBuff());
	if (second != OI_ERR_BUSY) ++bad;
	printf("hold pending limits bad=%d\n", bad);

	// 登记后序列化异常，以及发送异常与完成回调竞争；恢复后不能留下悬空等待项。
	const auto baseline = OI::Rpc::GetRpcWaitStats();
	auto balanced = [&]() {
		const auto now = OI::Rpc::GetRpcWaitStats();
		return now.calls == baseline.calls && now.expirations == baseline.expirations
			&& now.buckets == baseline.buckets;
	};
	struct ThrowingRequest : OI::RpcData::HoldReq
	{
		OI::Base::BytesStream& serialize(OI::Base::BytesStream&) const override
		{
			throw std::runtime_error("injected serialization failure");
		}
	} throwing_request;
	OI::RpcData::HoldReq& throwing_base = throwing_request;
	OI::RpcData::HoldRes unused;
	int caught_errors = 0;
	try { co_await OI::Rpc::RpcCall(pending, INVALID_SID, throwing_base, unused, OI::Timer::Milliseconds(50)); }
	catch (const std::runtime_error&) { ++caught_errors; }
	if (!balanced()) ++bad;
	pending.SetPendingCallLimits(64, 64 * 1024 * 1024);
	OI::RpcData::HoldReq ordinary;
	for (int mode = 1; mode <= 4; ++mode)
	{
		pending.failure_mode = mode;
		for (int i = 0; i < 128; ++i)
		{
			try { co_await OI::Rpc::RpcCall(pending, INVALID_SID, ordinary, unused, OI::Timer::Milliseconds(50)); }
			catch (const std::runtime_error&) { ++caught_errors; }
			if (!balanced()) ++bad;
		}
	}
	pending.failure_mode = 0;
	co_await coro::yield(OI::Timer::Milliseconds(250));
	if (caught_errors != 513 || !balanced()) ++bad;
	printf("hold rpc exceptions caught=%d bad=%d\n", caught_errors, bad);

	// 每个分片留一条长超时请求堵住队首，再拒绝十万条；索引数量必须保持不变。
	pending.SetPendingCallLimits(64, 64 * 8);
	int anchor_results[64]{};
	for (int i = 0; i < 64; ++i)
	{
		auto id = OI::Rpc::BindRpcCall(&pending, INVALID_SID, 0, {}, &anchor_results[i], 180000, {}, []() {});
		pending.TryPendCall(id, frame.GetMutableBuff());
	}
	const auto anchored = OI::Rpc::GetRpcWaitStats();
	if (anchored.calls != baseline.calls + 64 || anchored.expirations != baseline.expirations + 64) ++bad;
	for (int i = 0; i < 100000; ++i)
	{
		int rejected = 0;
		auto id = OI::Rpc::BindRpcCall(&pending, INVALID_SID, 0, {}, &rejected, 180000 + i % 17, {}, []() {});
		pending.TryPendCall(id, frame.GetMutableBuff());
		if (rejected != OI_ERR_BUSY) ++bad;
	}
	const auto limited = OI::Rpc::GetRpcWaitStats();
	if (limited.calls != anchored.calls || limited.expirations != anchored.expirations
		|| limited.buckets != anchored.buckets) ++bad;
	pending.FailHeldCalls();
	for (int code : anchor_results) if (code != OI_ERR_CONNECT) ++bad;
	if (!balanced()) ++bad;
	printf("hold rpc expiry rejected=100000 retained=%zu bad=%d\n", limited.expirations - baseline.expirations, bad);

	// 只在内部配对入口注入错误来源，不构造另一套网络客户端。
	auto& mgr = IpcClient::GetInstance();
	const auto response_id = ::OI::Rpc::RpcProtoId<::OI::RpcData::HoldRes>::value;
	int decoded = 0, resumed = 0, result = -99;
	uint64_t call = ::OI::Rpc::BindRpcCall(&mgr, _sid, response_id, {}, &result, 0,
		[&](::OI::Base::BytesStream&) { ++decoded; return true; }, [&]() { ++resumed; });
	auto deliver = [&](::OI::Net::SessionManager* source, sid_t sid) {
		::OI::Base::BytesStream body;
		body << call;
		::OI::Rpc::DeliverRpc(response_id, body, source, sid);
	};
	deliver(&mgr, _sid + 100000);
	deliver(&::OI::Redis::RedisClient::GetInstance(), _sid);
	if (decoded || resumed) ++bad;
	deliver(&mgr, _sid);
	if (decoded != 1 || resumed != 1) ++bad;
	resumed = 0;
	const sid_t fake_sid = _sid + 100000;
	::OI::Rpc::BindRpcCall(&mgr, fake_sid, response_id, {}, &result, 0, {}, [&]() { ++resumed; });
	::OI::Rpc::FailRpcCalls(&mgr, fake_sid);
	::OI::Rpc::FailRpcCalls(&mgr, fake_sid);
	if (result != OI_ERR_CONNECT || resumed != 1) ++bad;
	printf("hold rpc source and disconnect bad=%d\n", bad);

	::OI::RpcData::HoldReq req;
	::OI::RpcData::HoldRes res;

	req.delay_ms = 0;
	req.mark = 1;
	auto direct = co_await ::OI::Rpc::RpcCall(mgr, req, ::OI::Timer::Seconds(10));
	if (!direct || direct.value.mark != 1) ++bad;
	req.delay_ms = 600;
	auto short_wait = co_await ::OI::Rpc::RpcCall<::OI::RpcData::HoldRes>(mgr, _sid, req, ::OI::Timer::Milliseconds(150));
	if (short_wait.code != OI_ERR_TIMEOUT) ++bad;
	co_await coro::yield(::OI::Timer::Milliseconds(700));
	req.delay_ms = 0;
	int ret = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), _sid, req, res, 10);
	printf("hold rpc early ret=%d mark=%d\n", ret, res.mark);
	fflush(stdout);
	if (ret != 0 || res.mark != 1)
	{
		bad += 1;
	}

	std::clock_t cpu_begin = std::clock();
	auto wall_begin = std::chrono::steady_clock::now();
	req.delay_ms = 10000;
	req.mark = 2;
	res.mark = 0;
	ret = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), _sid, req, res, 2);
	printf("hold rpc timeout ret=%d\n", ret);
	fflush(stdout);
	if (ret != OI_ERR_TIMEOUT)
	{
		bad += 1;
	}
	// 回包还在路上。让这一下把迟到的包和已经武装的超时定时器都消化掉。
	co_await coro::yield(10000);
	printf("hold rpc late-reply cpu_ms=%lld wall_ms=%lld\n",
		CpuMs(cpu_begin), WallMs(wall_begin));
	fflush(stdout);

	req.delay_ms = 0;
	req.mark = 3;
	res.mark = 0;
	ret = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), _sid, req, res, 10);
	printf("hold rpc follow ret=%d mark=%d\n", ret, res.mark);
	fflush(stdout);
	if (ret != 0 || res.mark != 3)
	{
		bad += 1;
	}

	const int delays[] = {9800, 9900, 10000, 10100, 10200, 10400, 10600, 10800};
	const int n = 8;
	// 每个调用自己持有一份。没写完之前，这里退出也不会把盒子释放掉。
	std::shared_ptr<RaceBox> box(new RaceBox());
	sid_t sid = _sid;
	cpu_begin = std::clock();
	wall_begin = std::chrono::steady_clock::now();
	for (int i = 0; i < n; ++i)
	{
		int mark = 100 + i;
		int delay = delays[i];
		coro::StartTask([mark, delay, i, box, sid](coro::Token) {
			return RaceOne(sid, i, mark, delay, box);
		});
	}
	// 单次超时是 10 秒。等都写完再读，避免读到写了一半的结果。
	int waited = 0;
	const int kCapMs = 20000;
	while (box->finished.load(std::memory_order_acquire) < n && waited < kCapMs)
	{
		co_await coro::yield(50);
		waited += 50;
	}
	int done = box->finished.load(std::memory_order_acquire);
	int reply_n = 0;
	int timeout_n = 0;
	int race_bad = done == n ? 0 : 1;
	if (done == n)
	{
		for (int i = 0; i < n; ++i)
		{
			if (box->rets[i] == 0 && box->marks[i] == 100 + i)
			{
				reply_n += 1;
			}
			else if (box->rets[i] == OI_ERR_TIMEOUT)
			{
				timeout_n += 1;
			}
			else
			{
				race_bad += 1;
			}
		}
	}
	bad += race_bad;
	printf("hold rpc race n=%d done=%d reply=%d timeout=%d bad=%d cpu_ms=%lld wall_ms=%lld\n",
		n, done, reply_n, timeout_n, race_bad, CpuMs(cpu_begin), WallMs(wall_begin));
	fflush(stdout);

	int spins = 0;
	int limit = _redis_timeout_ms > 0 ? _redis_timeout_ms / 50 : 100;
	if (limit < 1)
	{
		limit = 1;
	}
	while (!::OI::Redis::RedisClient::GetInstance().IsConnect())
	{
		if (spins++ >= limit)
		{
			printf("hold redis connect fail\n");
			fflush(stdout);
			bad += 1;
			printf("hold done bad=%d\n", bad);
			fflush(stdout);
			TestBoard::GetInstance().Done(1);
			co_return;
		}
		co_await coro::yield(50);
	}

	::OI::Redis::RedisResult ping;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::CallRedis({"PING"}, ping);
		co_await call;
	}
	printf("hold redis ping ok=%d\n", Pong(ping) ? 1 : 0);
	fflush(stdout);
	if (!Pong(ping))
	{
		bad += 1;
	}

	int sec = _redis_timeout_ms / 1000;
	if (sec < 1)
	{
		sec = 1;
	}
	cpu_begin = std::clock();
	wall_begin = std::chrono::steady_clock::now();
	::OI::Redis::RedisResult late;
	{
		late = ::OI::Redis::CallRedisSync({"BLPOP", "hold-late", std::to_string(sec + 5)});
	}
	printf("hold redis late timeout=%d cpu_ms=%lld wall_ms=%lld\n",
		late.timeout ? 1 : 0, CpuMs(cpu_begin), WallMs(wall_begin));
	fflush(stdout);
	if (!late.timeout)
	{
		bad += 1;
	}
	::OI::Redis::RedisResult ping_after_late;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::CallRedis({"PING"}, ping_after_late);
		co_await call;
	}
	printf("hold redis late ping ok=%d\n", Pong(ping_after_late) ? 1 : 0);
	fflush(stdout);
	if (!Pong(ping_after_late))
	{
		bad += 1;
	}

	cpu_begin = std::clock();
	wall_begin = std::chrono::steady_clock::now();
	::OI::Redis::RedisResult race;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::CallRedis({"BLPOP", "hold-race", std::to_string(sec)}, race);
		co_await call;
	}
	int nil = race.reply && race.reply->type == REDIS_REPLY_NIL ? 1 : 0;
	printf("hold redis race timeout=%d nil=%d cpu_ms=%lld wall_ms=%lld\n",
		race.timeout ? 1 : 0, nil, CpuMs(cpu_begin), WallMs(wall_begin));
	fflush(stdout);
	if (!race.timeout && !nil)
	{
		bad += 1;
	}
	::OI::Redis::RedisResult ping_after_race;
	{
		::OI::Redis::RedisCall call = ::OI::Redis::CallRedis({"PING"}, ping_after_race);
		co_await call;
	}
	printf("hold redis race ping ok=%d\n", Pong(ping_after_race) ? 1 : 0);
	fflush(stdout);
	if (!Pong(ping_after_race))
	{
		bad += 1;
	}

	// 真断开当前会话：已发出的 RPC 应及时失败，不能继续等十秒超时。
	req.delay_ms = 5000;
	sid_t close_sid = _sid;
	coro::StartTask([close_sid](coro::Token) -> coro::Task {
		co_await coro::yield(::OI::Timer::Milliseconds(100));
		IpcClient::GetInstance().CloseSession(close_sid);
	});
	wall_begin = std::chrono::steady_clock::now();
	auto closed = co_await ::OI::Rpc::RpcCall<::OI::RpcData::HoldRes>(mgr, _sid, req, ::OI::Timer::Seconds(10));
	if (closed.code != OI_ERR_CONNECT || WallMs(wall_begin) > 2000) ++bad;
	printf("hold rpc disconnected code=%d wall_ms=%lld\n", closed.code, WallMs(wall_begin));
	printf("hold done bad=%d\n", bad);
	fflush(stdout);
	TestBoard::GetInstance().Done(bad == 0 ? 0 : 1);
	co_return;
}
