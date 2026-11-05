#include "redis_mgr.h"
#include "oi_redis.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_config.h"
#include "oi_net_session.h"
#include <atomic>
#include <memory>
#include <stdexcept>
#include "test_board.h"
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <uv.h>

RedisMgr::RedisMgr()
    : m_bEnabled(false), m_iTaskCount(8), m_iSynchronous(2), m_iSeconds(10), m_iTimeoutMilliseconds(25000), m_iFailed(0), m_iOperations(0), m_iBad(0)
{
}

void RedisMgr::Load()
{
	TryGetConfig("Test.redis", m_bEnabled);
	TryGetConfig("TestRedis.tasks", m_iTaskCount);
	TryGetConfig("TestRedis.seconds", m_iSeconds);
	TryGetConfig("TestRedis.timeout_ms", m_iTimeoutMilliseconds);
}

void RedisMgr::Start()
{
	if (!m_bEnabled)
	{
		return;
	}
	m_stStart = std::chrono::steady_clock::now();
	TestBoard::GetInstance().Arm();
	coro::StartTask([](coro::Token)
	{
		return RedisMgr::GetInstance().Pump();
	});
}

static bool StatusIs(const ::OI::Redis::RedisResult& stResultParam, const char* pTextParam)
{
	return !stResultParam.bTimeout && stResultParam.pReply && stResultParam.pReply->type == REDIS_REPLY_STATUS && stResultParam.pReply->str && std::strcmp(stResultParam.pReply->str, pTextParam) == 0;
}

static bool IntIs(const ::OI::Redis::RedisResult& stResultParam, long long iNumberParam)
{
	return !stResultParam.bTimeout && stResultParam.pReply && stResultParam.pReply->type == REDIS_REPLY_INTEGER && stResultParam.pReply->integer == iNumberParam;
}

static bool BulkIs(const ::OI::Redis::RedisResult& stResultParam, const char* pTextParam, size_t iNumberParam)
{
	return !stResultParam.bTimeout && stResultParam.pReply && stResultParam.pReply->type == REDIS_REPLY_STRING && stResultParam.pReply->len == iNumberParam && stResultParam.pReply->str && std::memcmp(stResultParam.pReply->str, pTextParam, iNumberParam) == 0;
}

static coro::Task RedisQueueTimeoutRegression()
{
	// 故意占用普通队列线程，超时必须仍由 poll 推进。
	if (!OI::Thread::ThreadPool::GetInstance().AddTask([]()
	{
		const auto stStart = std::chrono::steady_clock::now();
		auto stResult = OI::Redis::CallRedisSync({"BLPOP", "oi-queue-timeout-regression", "2"});
		const auto iElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stStart).count();
		const bool bGood = stResult.iStatus == OI::Redis::RedisStatus::Timeout && iElapsed < 1000 && OI::Redis::RedisClient::GetInstance().PendingCount() == 1;
		LOG_TRACE("redis queue timeout elapsed=%lld bad=%d", static_cast<long long>(iElapsed), bGood ? 0 : 1);
		fflush(stdout);
		TestBoard::GetInstance().Done(bGood ? 0 : 1);
	}, OI::Thread::PRIO_QUEUE))
	{
		TestBoard::GetInstance().Done(1);
	}
	co_return;
}

static coro::Task RedisShutdownRegression()
{
	if (!OI::Thread::ThreadPool::GetInstance().AddTask([]()
	{
		auto stResult = OI::Redis::CallRedisSync({"BLPOP", "oi-shutdown-regression", "0"});
		auto stLate = OI::Redis::RedisPingSync();
		const bool bGood = stResult.iStatus == OI::Redis::RedisStatus::Disconnected && stLate.iStatus == OI::Redis::RedisStatus::Disconnected && OI::Redis::RedisClient::GetInstance().PendingCount() == 0;
		LOG_TRACE("redis shutdown result=%d late=%d bad=%d", static_cast<int>(stResult.iStatus), static_cast<int>(stLate.iStatus), bGood ? 0 : 1);
		fflush(stdout);
	}))
	{
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	while (OI::Redis::RedisClient::GetInstance().PendingCount() == 0 && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await coro::yield(1);
	}
	if (OI::Redis::RedisClient::GetInstance().PendingCount() == 0)
	{
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	LOG_TRACE("redis shutdown pending");
	fflush(stdout);
	// 驱动脚本收到标记后发 SIGTERM；测试线程必须被停止回调唤醒。
}

static coro::TaskResult<int> PressureEcho(int iIndexParam)
{
	const std::string sPayload = std::to_string(iIndexParam) + std::string(256 * 1024, 'x');
	std::vector<std::string> vArguments{"ECHO", sPayload};
	auto stResult = co_await OI::Redis::CallRedis(std::move(vArguments));
	if (!stResult || !stResult.pReply->str || stResult.pReply->len != sPayload.size() || std::memcmp(stResult.pReply->str, sPayload.data(), sPayload.size()) != 0)
	{
		co_return 1;
	}
	co_return 0;
}

static coro::Task RedisPressureRegression(bool bOversizedParam)
{
	std::vector<coro::TaskResult<int>> vEchoes;
	for (int iIndex = 0; iIndex < 64; ++iIndex)
	{
		vEchoes.push_back(PressureEcho(iIndex));
	}
	int iBad = 0;
	for (const auto& stEcho : vEchoes)
	{
		iBad += co_await stEcho;
	}
	if (bOversizedParam)
	{
		auto& stClient = OI::Redis::RedisClient::GetInstance();
		const sid_t iOldSid = stClient.ConnectedSid();
		std::vector<std::string> vArguments{"ECHO", std::string(2 * 1024 * 1024, 'y')};
		auto stResult = co_await OI::Redis::CallRedis(std::move(vArguments));
		if (stResult.iStatus != OI::Redis::RedisStatus::Disconnected)
		{
			++iBad;
		}
		const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
		while ((!stClient.IsConnect() || stClient.ConnectedSid() == iOldSid) && ::OI::Timer::SteadyMilliseconds() < iDeadline)
		{
			co_await coro::yield(1);
		}
	}
	if (!StatusIs(co_await OI::Redis::RedisPing(), "PONG"))
	{
		++iBad;
	}
	LOG_TRACE("redis pressure done total=64 bad=%d", iBad);
	fflush(stdout);
	TestBoard::GetInstance().Done(iBad ? 1 : 0);
}

// 回归脚本按端口偏移改写配置，直连 redisserver 的探针必须跟着配置走。
static std::string RedisServerPort()
{
	std::string sPort = "16379";
	TryGetConfig("RedisClient.port", sPort);
	return sPort;
}

// 两条真实下游连接共享一条上游；生命周期覆盖整个测试进程。
class RedisFairPeer : public OI::Net::SessionManager
{
	class ReplySession : public OI::Net::NetSession
	{
		RedisFairPeer* m_pOwner;

	  public:
		explicit ReplySession(RedisFairPeer* pOwnerParam)
		    : NetSession(pOwnerParam), m_pOwner(pOwnerParam)
		{
		}
		void OnDataOut() override
		{
		}
		void OnDataIn() override
		{
			while (stInputBuffer.Size() >= m_pOwner->m_sExpected.size())
			{
				const auto& sExpected = m_pOwner->m_sExpected;
				if (std::memcmp(stInputBuffer.Begin(), sExpected.data(), sExpected.size()) != 0)
				{
					++m_pOwner->m_iBad;
				}
				stInputBuffer.Erase(0, sExpected.size());
				++m_pOwner->m_iReplies;
				if (m_pOwner->m_pOther)
				{
					m_pOwner->m_iOtherAtReply = m_pOwner->m_pOther->m_iReplies.load();
				}
			}
		}
	};
	std::string m_sLabel;
	std::string m_sExpected;
	RedisFairPeer* m_pOther;
	std::atomic<int> m_iReplies{0}, m_iBad{0}, m_iOtherAtReply{0};
	std::atomic<sid_t> m_iSid{INVALID_SID};
	OI::Net::Session* CloneSession() override
	{
		return new ReplySession(this);
	}
	void OnAddSession(OI::Net::Session* pSessionParam) override
	{
		m_iSid = pSessionParam->GetSessionID();
	}
	void OnDelSession(const OI::Net::Session*) override
	{
		m_iSid = INVALID_SID;
	}

  public:
	RedisFairPeer(std::string sLabelParam, RedisFairPeer* pOtherParam, std::string sExpectedParam = "")
	    : m_sLabel(std::move(sLabelParam)), m_sExpected(sExpectedParam.empty() ? std::string("$1\r\n") + m_sLabel + "\r\n" : std::move(sExpectedParam)), m_pOther(pOtherParam)
	{
		DefaultInit("127.0.0.1", RedisServerPort());
	}
	bool SendCommands(int iCountParam)
	{
		OI::Base::Bytes stWire;
		const std::string sCommand = std::string("*2\r\n$4\r\nECHO\r\n$1\r\n") + m_sLabel + "\r\n";
		for (int iIndex = 0; iIndex < iCountParam; ++iIndex)
		{
			stWire.Append(sCommand.data(), sCommand.size());
		}
		return SendBytes(m_iSid.load(), stWire);
	}
	bool SendScriptProbe()
	{
		const std::string sCommand = "*3\r\n$4\r\nEVAL\r\n$9\r\nreturn 42\r\n$1\r\n0\r\n";
		OI::Base::Bytes stWire;
		stWire.Append(sCommand.data(), sCommand.size());
		return SendBytes(m_iSid.load(), stWire);
	}
	int Replies() const
	{
		return m_iReplies.load();
	}
	int Bad() const
	{
		return m_iBad.load();
	}
	int OtherAtReply() const
	{
		return m_iOtherAtReply.load();
	}
};

static coro::Task RedisFairnessRegression()
{
	static RedisFairPeer stFirst("A", nullptr), stSecond("B", &stFirst);
	int iBad = 0;
	if (!(co_await stFirst.StartClient()) || !stFirst.SendCommands(64))
	{
		++iBad;
	}
	co_await coro::yield(100);
	if (!(co_await stSecond.StartClient()) || !stSecond.SendCommands(1))
	{
		++iBad;
	}
	LOG_TRACE("redis fairness ready");
	fflush(stdout);
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(8)).iMilliseconds;
	while (stFirst.Replies() == 0 && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await coro::yield(1);
	}
	const auto stStart = std::chrono::steady_clock::now();
	while ((stFirst.Replies() != 64 || stSecond.Replies() != 1) && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await coro::yield(1);
	}
	const auto iElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stStart).count();
	if (stFirst.Replies() != 64 || stSecond.Replies() != 1 || iElapsed > 1500)
	{
		++iBad;
	}
	iBad += stFirst.Bad() + stSecond.Bad();
	stFirst.CloseAllSessions();
	stSecond.CloseAllSessions();
	LOG_TRACE("redis fairness done first=%d second=%d other_at_reply=%d elapsed_ms=%lld bad=%d", stFirst.Replies(), stSecond.Replies(), stSecond.OtherAtReply(), static_cast<long long>(iElapsed), iBad);
	fflush(stdout);
	TestBoard::GetInstance().Done(iBad ? 1 : 0);
}

coro::Task RedisMgr::Pump()
{
	int iSpins = 0;
	int iLimit = m_iTimeoutMilliseconds > 0 ? m_iTimeoutMilliseconds / 50 : 100;
	if (iLimit < 1)
	{
		iLimit = 1;
	}
	while (!::OI::Redis::RedisClient::GetInstance().IsConnect())
	{
		if (iSpins++ >= iLimit)
		{
			LOG_TRACE("redis connect timeout");
			TestBoard::GetInstance().Done(1);
			co_return;
		}
		co_await coro::yield(50);
	}

	bool bShutdown = false, bPressure = false, bOversized = false, bFairness = false;
	bool bQueueTimeout = false;
	TryGetConfig("Regression.redis_queue_timeout", bQueueTimeout);
	if (bQueueTimeout)
	{
		co_await RedisQueueTimeoutRegression();
		co_return;
	}
	TryGetConfig("Regression.redis_fairness", bFairness);
	if (bFairness)
	{
		co_await RedisFairnessRegression();
		co_return;
	}
	TryGetConfig("Regression.redis_shutdown", bShutdown);
	TryGetConfig("Regression.redis_pressure", bPressure);
	TryGetConfig("Regression.redis_oversized", bOversized);
	if (bShutdown)
	{
		co_await RedisShutdownRegression();
		co_return;
	}
	if (bPressure)
	{
		co_await RedisPressureRegression(bOversized);
		co_return;
	}
	m_iFailed = 0;
	co_await Check();
	if (m_iFailed)
	{
		LOG_TRACE("redis integrity failed=%d", m_iFailed);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	LOG_TRACE("redis integrity ok");
	m_stStart = std::chrono::steady_clock::now();
	if (m_iTaskCount <= 0 || m_iSeconds <= 0)
	{
		TestBoard::GetInstance().Done(0);
		co_return;
	}
	std::vector<coro::Task> vStress;
	for (int iIndexLocal = 0; iIndexLocal < m_iTaskCount; ++iIndexLocal)
	{
		vStress.push_back(Stress(iIndexLocal));
	}
#ifdef _SINGLE_THREAD_
	// 收包只有这一条线程。另开线程再 join，poll 被堵住，同步调用永远等不到回包。
	for (int iIndexLocal = 0; iIndexLocal < m_iSynchronous; ++iIndexLocal)
	{
		SyncStress(iIndexLocal);
	}
#else
	std::vector<std::thread> vSyncs;
	for (int iIndexLocal = 0; iIndexLocal < m_iSynchronous; ++iIndexLocal)
	{
		vSyncs.emplace_back(&RedisMgr::SyncStress, this, iIndexLocal);
	}
	for (auto& stSync : vSyncs)
	{
		stSync.join();
	}
#endif
	for (const auto& stTask : vStress)
	{
		co_await stTask;
	}
	Report();
}

static coro::TaskResult<::OI::Redis::RedisResult> AwaitRedis(std::vector<std::string> vArgumentsParam)
{
	co_return co_await ::OI::Redis::CallRedis(std::move(vArgumentsParam));
}

// max_pending 为 1 时，第二条命令必须立即返回 Busy，前一条完成后恢复。
static coro::TaskResult<int> CheckPendingLimit()
{
	int iBad = 0;
	auto stBlockedTask = AwaitRedis({"BLPOP", "oi-limit-empty", "1"});
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	while (::OI::Redis::RedisClient::GetInstance().PendingCount() == 0 && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await coro::yield(::OI::Timer::Milliseconds(10));
	}
	auto stBusy = co_await ::OI::Redis::RedisPing();
	if (stBusy.iStatus != ::OI::Redis::RedisStatus::Busy || stBusy.bTimeout)
	{
		++iBad;
	}
	const auto stBlockedResult = co_await stBlockedTask;
	if (!stBlockedResult || !stBlockedResult.pReply || stBlockedResult.pReply->type != REDIS_REPLY_NIL)
	{
		++iBad;
	}
	if (!StatusIs(co_await ::OI::Redis::RedisPing(), "PONG"))
	{
		++iBad;
	}
	LOG_TRACE("redis pending limit bad=%d", iBad);
	co_return iBad;
}

static coro::TaskResult<int> CheckReplyStatus()
{
	int iBad = 0;
	auto stInvalid = co_await ::OI::Redis::CallRedis({});
	if (stInvalid.iStatus != ::OI::Redis::RedisStatus::InvalidArgument || stInvalid.bTimeout)
	{
		++iBad;
	}
	auto stDirect = co_await ::OI::Redis::RedisPing();
	if (!stDirect || !StatusIs(stDirect, "PONG"))
	{
		++iBad;
	}
	auto stErrorLocal = co_await ::OI::Redis::CallRedis({"OI_UNKNOWN_COMMAND"});
	if (stErrorLocal.iStatus != ::OI::Redis::RedisStatus::ServerError || !stErrorLocal.pReply || stErrorLocal.bTimeout)
	{
		++iBad;
	}
	co_return iBad;
}

// poll 线程里调用同步接口必须抛错，不能把自己堵死；协程里的同步调用照常返回。
static coro::TaskResult<int> CheckSyncOnLoop()
{
	int iBad = 0;
	std::atomic<int32_t> iSyncGuard(0);
	::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgumentParam)
	{
		auto pTimer = new uv_timer_t{};
		pTimer->data = pArgumentParam;
		uv_timer_init(OI::Net::Poll::GetInstance().Loop(), pTimer);
		uv_timer_start(pTimer, [](uv_timer_t* pTimerParam)
		{
			int32_t iChecked = -1;
			try
			{
				OI::Redis::RedisPingSync();
			}
			catch (const std::logic_error&)
			{
				iChecked = 1;
			}
			static_cast<std::atomic<int32_t>*>(pTimerParam->data)->store(iChecked, std::memory_order_release);
			uv_close(reinterpret_cast<uv_handle_t*>(pTimerParam), [](uv_handle_t* pHandleParam)
			{
				delete reinterpret_cast<uv_timer_t*>(pHandleParam);
			});
		}, 1, 0);
	}, &iSyncGuard);
	while (iSyncGuard.load(std::memory_order_acquire) == 0)
	{
		co_await coro::yield(10);
	}
	if (iSyncGuard.load() != 1)
	{
		++iBad;
	}
	if (!StatusIs(::OI::Redis::RedisPingSync(), "PONG"))
	{
		++iBad;
	}
	co_return iBad;
}

static coro::TaskResult<bool> AwaitSingleUse(std::shared_ptr<OI::Redis::RedisCall> pCallParam)
{
	try
	{
		if (IntIs(co_await *pCallParam, 1))
		{
			co_return true;
		}
	}
	catch (const std::logic_error&)
	{
	}
	co_return false;
}

// 同一个等待对象只能提交一次；重复或并发 await 必须在发送前拒绝，不能重复写 Redis。
static coro::TaskResult<int> CheckSingleUse()
{
	int iBad = 0;
	std::vector<std::string> vSingleUseKeys{"DEL", "oi-single-use", "oi-concurrent-use"};
	if (!(co_await OI::Redis::CallRedis(std::move(vSingleUseKeys))))
	{
		++iBad;
	}
	auto stSingleUse = OI::Redis::RedisIncr("oi-single-use");
	if (!IntIs(co_await stSingleUse, 1))
	{
		++iBad;
	}
	try
	{
		(void)(co_await stSingleUse);
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	if (!BulkIs(co_await OI::Redis::RedisGet("oi-single-use"), "1", 1))
	{
		++iBad;
	}
	auto pSharedCall = std::make_shared<OI::Redis::RedisCall>(OI::Redis::RedisIncr("oi-concurrent-use"));
	auto pReadyCount = std::make_shared<std::atomic<int>>(0);
	auto fnContend = [pSharedCall, pReadyCount](coro::Token) -> coro::TaskResult<bool>
	{
		pReadyCount->fetch_add(1, std::memory_order_acq_rel);
		// 两个独立任务都就绪后再争抢；单线程等待时让出调度。
		while (pReadyCount->load(std::memory_order_acquire) < 2)
		{
			co_await coro::yield(0);
		}
		co_return co_await AwaitSingleUse(pSharedCall);
	};
	auto stFirst = coro::StartTask(fnContend);
	auto stSecond = coro::StartTask(fnContend);
	const int iAccepted = (co_await stFirst ? 1 : 0) + (co_await stSecond ? 1 : 0);
	if (iAccepted != 1 || !BulkIs(co_await OI::Redis::RedisGet("oi-concurrent-use"), "1", 1))
	{
		++iBad;
	}
	std::vector<std::string> vCleanupKeys{"DEL", "oi-single-use", "oi-concurrent-use"};
	if (!(co_await OI::Redis::CallRedis(std::move(vCleanupKeys))))
	{
		++iBad;
	}
	LOG_TRACE("redis single use bad=%d", iBad);
	co_return iBad;
}

static coro::TaskResult<int> CheckCommands()
{
	int iBad = 0;
	// 计数和新建字段的断言依赖空键。上一轮留下的值会让 HSET 返回 0、INCR 从中间开始。
	auto stWiped = co_await ::OI::Redis::CallRedis({"DEL", "rk", "rbin", "rk-missing", "rk-exp", "rh", "rl", "rs", "rk-count"});
	if (stWiped.bTimeout || !stWiped.pReply || stWiped.pReply->type != REDIS_REPLY_INTEGER)
	{
		++iBad;
	}
	if (!StatusIs(co_await ::OI::Redis::RedisSet("rk", "hello"), "OK"))
	{
		++iBad;
	}
	if (!BulkIs(co_await ::OI::Redis::RedisGet("rk"), "hello", 5))
	{
		++iBad;
	}
	const std::string sBin("a\0b", 3);
	if (!StatusIs(co_await ::OI::Redis::RedisSet("rbin", sBin), "OK"))
	{
		++iBad;
	}
	if (!BulkIs(co_await ::OI::Redis::RedisGet("rbin"), sBin.data(), 3))
	{
		++iBad;
	}
	const auto stMiss = co_await ::OI::Redis::RedisGet("rk-missing");
	if (stMiss.bTimeout || !stMiss.pReply || stMiss.pReply->type != REDIS_REPLY_NIL)
	{
		++iBad;
	}
	if (!IntIs(co_await ::OI::Redis::RedisDel("rk"), 1))
	{
		++iBad;
	}
	if (!IntIs(co_await ::OI::Redis::RedisHSet("rh", "f", "v"), 1))
	{
		++iBad;
	}
	if (!BulkIs(co_await ::OI::Redis::RedisHGet("rh", "f"), "v", 1))
	{
		++iBad;
	}
	co_await ::OI::Redis::RedisLPush("rl", "a");
	co_await ::OI::Redis::RedisLPush("rl", "b");
	const auto stLrange = co_await ::OI::Redis::RedisLRange("rl", 0, 1);
	if (stLrange.bTimeout || !stLrange.pReply || stLrange.pReply->type != REDIS_REPLY_ARRAY || stLrange.pReply->elements != 2)
	{
		++iBad;
	}
	co_await ::OI::Redis::RedisSAdd("rs", "m");
	if (!IntIs(co_await ::OI::Redis::RedisSIsMember("rs", "m"), 1))
	{
		++iBad;
	}
	// 输出引用接口仍对外保留，这里覆盖它。
	for (int iIndexLocal = 1; iIndexLocal <= 20; ++iIndexLocal)
	{
		::OI::Redis::RedisResult stNumber;
		::OI::Redis::RedisCall stCall = ::OI::Redis::RedisIncr("rk-count", stNumber);
		co_await stCall;
		if (!IntIs(stNumber, iIndexLocal))
		{
			++iBad;
			break;
		}
	}
	co_return iBad;
}

static coro::TaskResult<int> CheckExpiry()
{
	int iBad = 0;
	if (!StatusIs(co_await ::OI::Redis::RedisSet("rk-exp", "1"), "OK"))
	{
		++iBad;
	}
	if (!IntIs(co_await ::OI::Redis::RedisExpire("rk-exp", ::OI::Timer::Seconds(60)), 1))
	{
		++iBad;
	}
	auto stMilliseconds = co_await ::OI::Redis::RedisExpire("rk-exp", ::OI::Timer::Milliseconds(750));
	auto stRemaining = co_await ::OI::Redis::CallRedis({"PTTL", "rk-exp"});
	const bool bMillisecondExpiry = IntIs(stMilliseconds, 1) && stRemaining.pReply && stRemaining.pReply->type == REDIS_REPLY_INTEGER && stRemaining.pReply->integer > 0 && stRemaining.pReply->integer <= 750;
	LOG_TRACE("redis millisecond expiry bad=%d", bMillisecondExpiry ? 0 : 1);
	if (!bMillisecondExpiry)
	{
		++iBad;
	}
	co_return iBad;
}

// runner 将普通 Redis 用例配置成单条上游，两条下游必须互不污染。
static coro::TaskResult<int> CheckScriptIsolation()
{
	static RedisFairPeer stScriptPeer("B", nullptr, ":42\r\n");
	int iBad = 0;
	if (!(co_await stScriptPeer.StartClient()))
	{
		++iBad;
	}
	const std::vector<std::vector<std::string>> vDebugCommands{
	    {"sCrIpT", "dEbUg", "YES"}, {"SCRIPT", "DEBUG", "SYNC"}, {"script", "debug", "NO"}, {"SCRIPT", std::string("DEBUG\0suffix", 12), "SYNC"},
	    {"PMONITOR"}, {"pMoNiToR"}, {"MONITOR\r\nignored"}, {"PMONITOR\r\nignored"},
	    {"SUBSCRIBE\r\nignored", "topic"}, {"PSUBSCRIBE\r\nignored", "topic"},
	    {"UNSUBSCRIBE\r\nignored"}, {"PUNSUBSCRIBE\r\nignored"}};
	for (const auto& vArguments : vDebugCommands)
	{
		auto stRejected = co_await OI::Redis::CallRedis(vArguments);
		if (stRejected.iStatus != OI::Redis::RedisStatus::Disconnected)
		{
			++iBad;
		}
		const int iExpected = stScriptPeer.Replies() + 1;
		if (!stScriptPeer.SendScriptProbe())
		{
			++iBad;
		}
		const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
		while (stScriptPeer.Replies() < iExpected && ::OI::Timer::SteadyMilliseconds() < iDeadline)
		{
			co_await coro::yield(1);
		}
		if (stScriptPeer.Replies() != iExpected || stScriptPeer.Bad() != 0)
		{
			++iBad;
		}
		if (!(co_await OI::Redis::RedisClient::GetInstance().StartClient()))
		{
			++iBad;
		}
		if (!StatusIs(co_await OI::Redis::RedisPing(), "PONG"))
		{
			++iBad;
		}
	}
	std::vector<std::string> vLoad{"SCRIPT", "LOAD", "return 42"};
	auto stLoaded = co_await OI::Redis::CallRedis(std::move(vLoad));
	if (!stLoaded || !stLoaded.pReply->str || stLoaded.pReply->type != REDIS_REPLY_STRING)
	{
		++iBad;
	}
	else
	{
		std::vector<std::string> vEval{"EVALSHA", std::string(stLoaded.pReply->str, stLoaded.pReply->len), "0"};
		if (!IntIs(co_await OI::Redis::CallRedis(std::move(vEval)), 42))
		{
			++iBad;
		}
	}
	stScriptPeer.CloseAllSessions();
	LOG_TRACE("redis script isolation bad=%d", iBad);
	co_return iBad;
}

// 状态命令必须被代理拒绝；重连后普通命令仍能使用，不能留下事务或切库状态。
static coro::TaskResult<int> CheckDeniedCommands()
{
	int iBad = 0;
	const char* vDenied[] = {"SELECT", "MULTI", "CLIENT", "SUBSCRIBE"};
	for (const char* pCommand : vDenied)
	{
		std::vector<std::string> vCommand{pCommand, "1"};
		const auto stRejected = co_await ::OI::Redis::CallRedis(std::move(vCommand));
		if (stRejected.iStatus != ::OI::Redis::RedisStatus::Disconnected || stRejected.bTimeout)
		{
			++iBad;
		}
		int iAttempts = 0;
		while (!::OI::Redis::RedisClient::GetInstance().IsConnect() && iAttempts++ < 100)
		{
			co_await coro::yield(50);
		}
		if (!StatusIs(co_await ::OI::Redis::RedisPing(), "PONG"))
		{
			++iBad;
		}
	}
	co_return iBad;
}

coro::Task RedisMgr::Check()
{
	int32_t iPendingLimit = 65536;
	TryGetConfig("RedisClient.max_pending", iPendingLimit);
	if (iPendingLimit == 1)
	{
		m_iFailed += co_await CheckPendingLimit();
		co_return;
	}
	m_iFailed += co_await CheckReplyStatus();
	m_iFailed += co_await CheckSyncOnLoop();
	m_iFailed += co_await CheckSingleUse();
	m_iFailed += co_await CheckCommands();
	m_iFailed += co_await CheckExpiry();
	m_iFailed += co_await CheckScriptIsolation();
	m_iFailed += co_await CheckDeniedCommands();
}

void RedisMgr::Report()
{
	long long iMillisecondsLocal = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - m_stStart)
	                                   .count();
	if (iMillisecondsLocal < 1)
	{
		iMillisecondsLocal = 1;
	}
	long long iOperations = m_iOperations.load();
	int iBadLocal = m_iBad.load();
	LOG_TRACE("redis stress tasks=%d sync=%d ops=%lld ms=%lld rate=%lld/s bad=%d",
	    m_iTaskCount, m_iSynchronous, iOperations, iMillisecondsLocal, iOperations * 1000 / iMillisecondsLocal, iBadLocal);
	TestBoard::GetInstance().Done(iBadLocal == 0 ? 0 : 1);
}

void RedisMgr::SyncStress(int iIdParam)
{
	std::string sKey = "sync:" + std::to_string(iIdParam);
	auto stEnd = m_stStart + std::chrono::seconds(m_iSeconds);
	while (std::chrono::steady_clock::now() < stEnd)
	{
		::OI::Redis::RedisResult stPut = ::OI::Redis::RedisSetSync(sKey, "s");
		::OI::Redis::RedisResult stGot = ::OI::Redis::RedisGetSync(sKey);
		if (!StatusIs(stPut, "OK") || !BulkIs(stGot, "s", 1))
		{
			m_iBad.fetch_add(1);
		}
		else
		{
			m_iOperations.fetch_add(2);
		}
	}
}

coro::Task RedisMgr::Stress(int iIdParam)
{
	std::string sValueLocal(64, 'x');
	std::string sHash = "h:" + std::to_string(iIdParam);
	std::string sList = "l:" + std::to_string(iIdParam);
	auto stEnd = m_stStart + std::chrono::seconds(m_iSeconds);
	int iSequence = 0;
	int iKind = iIdParam % 4;
	while (std::chrono::steady_clock::now() < stEnd)
	{
		std::string sKey = "k:" + std::to_string(iIdParam) + ":" + std::to_string(iSequence % 256);
		std::string sMember = std::to_string(iSequence % 32);
		++iSequence;
		::OI::Redis::RedisResult stResultLocal;
		bool bOk = false;
		if (iKind == 0)
		{
			stResultLocal = co_await ::OI::Redis::RedisSet(sKey, sValueLocal);
			bOk = StatusIs(stResultLocal, "OK");
		}
		else if (iKind == 1)
		{
			stResultLocal = co_await ::OI::Redis::RedisHSet(sHash, sMember, sValueLocal);
			bOk = IntIs(stResultLocal, 0) || IntIs(stResultLocal, 1);
		}
		else if (iKind == 2)
		{
			stResultLocal = co_await ::OI::Redis::RedisLPush(sList, sMember);
			bOk = !stResultLocal.bTimeout && stResultLocal.pReply && stResultLocal.pReply->type == REDIS_REPLY_INTEGER;
			if (bOk && (iSequence % 16) == 0)
			{
				auto stTrim = co_await ::OI::Redis::RedisLTrim(sList, 0, 15);
				if (!StatusIs(stTrim, "OK"))
				{
					bOk = false;
				}
			}
		}
		else
		{
			stResultLocal = co_await ::OI::Redis::RedisIncr(sKey);
			bOk = !stResultLocal.bTimeout && stResultLocal.pReply && stResultLocal.pReply->type == REDIS_REPLY_INTEGER;
		}
		if (!bOk)
		{
			m_iBad.fetch_add(1);
		}
		else
		{
			m_iOperations.fetch_add(1);
		}
	}
}
