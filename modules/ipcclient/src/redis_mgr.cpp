#include "redis_mgr.h"
#include "oi_redis_cmd.h"
#include "oi_rpcdata_reginstance.h"
#include "oi_log.h"
#include "oi_poll.h"
#include "oi_config.h"
#include "oi_net_session.h"
#include <atomic>
#include <memory>
#include <set>
#include <stdexcept>
#include "test_board.h"
#include <cstring>
#include <fstream>
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
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return RedisMgr::GetInstance().Pump();
	});
}

static bool StatusIs(const ::OI::Redis::RedisResult& stResult, const char* pText)
{
	return !stResult.bTimeout && stResult.pReply && stResult.pReply->type == REDIS_REPLY_STATUS && stResult.pReply->str && std::strcmp(stResult.pReply->str, pText) == 0;
}

static bool IntIs(const ::OI::Redis::RedisResult& stResult, long long iNumber)
{
	return !stResult.bTimeout && stResult.pReply && stResult.pReply->type == REDIS_REPLY_INTEGER && stResult.pReply->integer == iNumber;
}

static bool BulkIs(const ::OI::Redis::RedisResult& stResult, const char* pText, size_t iNumber)
{
	return !stResult.bTimeout && stResult.pReply && stResult.pReply->type == REDIS_REPLY_STRING && stResult.pReply->len == iNumber && stResult.pReply->str && std::memcmp(stResult.pReply->str, pText, iNumber) == 0;
}

static OI::Coro::Task RedisQueueTimeoutRegression()
{
	// 故意在协程里同步阻塞，超时必须仍由 poll 推进。
	OI::Coro::StartTask([](OI::Coro::Token) -> OI::Coro::Task
	{
		const auto stStart = std::chrono::steady_clock::now();
		auto stResult = OI::Redis::CallRedisSync({"BLPOP", "oi-queue-timeout-regression", "2"});
		const auto iElapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - stStart).count();
		const bool bGood = stResult.iStatus == OI::Redis::RedisStatus::Timeout && stResult.GetRet() == OI_ERR_TIMEOUT && iElapsed < 1000 && OI::Redis::RedisClient::GetInstance().PendingCount() == 1;
		LOG_TRACE("redis queue timeout elapsed=%lld bad=%d", static_cast<long long>(iElapsed), bGood ? 0 : 1);
		fflush(stdout);
		TestBoard::GetInstance().Done(bGood ? 0 : 1);
		co_return;
	});
	co_return;
}

static OI::Coro::Task RedisShutdownRegression()
{
	OI::Coro::StartTask([](OI::Coro::Token) -> OI::Coro::Task
	{
		auto stResult = OI::Redis::CallRedisSync({"BLPOP", "oi-shutdown-regression", "0"});
		auto stLate = OI::Redis::CallRedisSync({"PING"});
		const bool bGood = stResult.iStatus == OI::Redis::RedisStatus::Disconnected && stLate.iStatus == OI::Redis::RedisStatus::Disconnected && stLate.GetRet() == OI_ERR_CONNECT && OI::Redis::RedisClient::GetInstance().PendingCount() == 0;
		LOG_TRACE("redis shutdown result=%d late=%d bad=%d", static_cast<int>(stResult.iStatus), static_cast<int>(stLate.iStatus), bGood ? 0 : 1);
		fflush(stdout);
		co_return;
	});
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	while (OI::Redis::RedisClient::GetInstance().PendingCount() == 0 && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await OI::Coro::yield(1);
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

static OI::Coro::TaskResult<int> PressureEcho(int iIndex)
{
	const std::string sPayload = std::to_string(iIndex) + std::string(256 * 1024, 'x');
	std::vector<std::string> vArguments{"ECHO", sPayload};
	auto stResult = co_await OI::Redis::CallRedis(std::move(vArguments));
	if (!stResult || !stResult.pReply->str || stResult.pReply->len != sPayload.size() || std::memcmp(stResult.pReply->str, sPayload.data(), sPayload.size()) != 0)
	{
		co_return 1;
	}
	co_return 0;
}

static OI::Coro::Task RedisPressureRegression(bool bOversized)
{
	std::vector<OI::Coro::TaskResult<int>> vEchoes;
	for (int iIndex = 0; iIndex < 64; ++iIndex)
	{
		vEchoes.push_back(PressureEcho(iIndex));
	}
	int iBad = 0;
	for (const auto& stEcho : vEchoes)
	{
		iBad += co_await stEcho;
	}
	if (bOversized)
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
			co_await OI::Coro::yield(1);
		}
	}
	if (!StatusIs(co_await OI::Redis::CallRedis({"PING"}), "PONG"))
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
		explicit ReplySession(RedisFairPeer* pOwner)
		    : NetSession(pOwner), m_pOwner(pOwner)
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
	void OnAddSession(OI::Net::Session* pSession) override
	{
		m_iSid = pSession->GetSessionID();
	}
	void OnDelSession(const OI::Net::Session*) override
	{
		m_iSid = INVALID_SID;
	}

  public:
	RedisFairPeer(std::string sLabel, RedisFairPeer* pOther, std::string sExpected = "")
	    : m_sLabel(std::move(sLabel)), m_sExpected(sExpected.empty() ? std::string("$1\r\n") + m_sLabel + "\r\n" : std::move(sExpected)), m_pOther(pOther)
	{
		DefaultInit("127.0.0.1", RedisServerPort());
	}
	bool SendCommands(int iCount)
	{
		OI::Base::Bytes stWire;
		const std::string sCommand = std::string("*2\r\n$4\r\nECHO\r\n$1\r\n") + m_sLabel + "\r\n";
		for (int iIndex = 0; iIndex < iCount; ++iIndex)
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

static OI::Coro::Task RedisFairnessRegression()
{
	static RedisFairPeer stFirst("A", nullptr), stSecond("B", &stFirst);
	int iBad = 0;
	if (!(co_await stFirst.StartClient()) || !stFirst.SendCommands(64))
	{
		++iBad;
	}
	co_await OI::Coro::yield(100);
	if (!(co_await stSecond.StartClient()) || !stSecond.SendCommands(1))
	{
		++iBad;
	}
	LOG_TRACE("redis fairness ready");
	fflush(stdout);
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(8)).iMilliseconds;
	while (stFirst.Replies() == 0 && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await OI::Coro::yield(1);
	}
	const auto stStart = std::chrono::steady_clock::now();
	while ((stFirst.Replies() != 64 || stSecond.Replies() != 1) && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await OI::Coro::yield(1);
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

OI::Coro::Task RedisMgr::Pump()
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
		co_await OI::Coro::yield(50);
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
	std::vector<OI::Coro::Task> vStress;
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

static OI::Coro::TaskResult<::OI::Redis::RedisResult> AwaitRedis(std::vector<std::string> vArguments)
{
	co_return co_await ::OI::Redis::CallRedis(std::move(vArguments));
}

// max_pending 为 1 时，第二条命令必须立即返回 Busy，前一条完成后恢复。
static OI::Coro::TaskResult<int> CheckPendingLimit()
{
	int iBad = 0;
	auto stBlockedTask = AwaitRedis({"BLPOP", "oi-limit-empty", "1"});
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	while (::OI::Redis::RedisClient::GetInstance().PendingCount() == 0 && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await OI::Coro::yield(::OI::Timer::Milliseconds(10));
	}
	auto stBusy = co_await ::OI::Redis::CallRedis({"PING"});
	if (stBusy.iStatus != ::OI::Redis::RedisStatus::Busy || stBusy.GetRet() != OI_ERR_BUSY || stBusy.bTimeout)
	{
		++iBad;
	}
	const auto stBlockedResult = co_await stBlockedTask;
	if (!stBlockedResult || !stBlockedResult.pReply || stBlockedResult.pReply->type != REDIS_REPLY_NIL)
	{
		++iBad;
	}
	if (!StatusIs(co_await ::OI::Redis::CallRedis({"PING"}), "PONG"))
	{
		++iBad;
	}
	LOG_TRACE("redis pending limit bad=%d", iBad);
	co_return iBad;
}

static OI::Coro::TaskResult<int> CheckReplyStatus()
{
	int iBad = 0;
	auto stInvalid = co_await ::OI::Redis::CallRedis({});
	if (stInvalid.iStatus != ::OI::Redis::RedisStatus::InvalidArgument || stInvalid.GetRet() != OI_ERR_INVALID_ARGUMENT || stInvalid.bTimeout)
	{
		++iBad;
	}
	auto stDirect = co_await ::OI::Redis::CallRedis({"PING"});
	if (!stDirect || !StatusIs(stDirect, "PONG"))
	{
		++iBad;
	}
	auto stErrorLocal = co_await ::OI::Redis::CallRedis({"OI_UNKNOWN_COMMAND"});
	if (stErrorLocal.iStatus != ::OI::Redis::RedisStatus::ServerError || stErrorLocal.GetRet() != OI_ERR_REDIS || !stErrorLocal.pReply || stErrorLocal.bTimeout)
	{
		++iBad;
	}
	co_return iBad;
}

// poll 线程里调用同步接口必须抛错，不能把自己堵死；协程里的同步调用照常返回。
static OI::Coro::TaskResult<int> CheckSyncOnLoop()
{
	int iBad = 0;
	std::atomic<int32_t> iSyncGuard(0);
	::OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgument)
	{
		auto pTimer = new uv_timer_t{};
		pTimer->data = pArgument;
		uv_timer_init(OI::Net::Poll::GetInstance().Loop(), pTimer);
		uv_timer_start(pTimer, [](uv_timer_t* pTimer)
		{
			int32_t iChecked = -1;
			try
			{
				OI::Redis::CallRedisSync({"PING"});
			}
			catch (const std::logic_error&)
			{
				iChecked = 1;
			}
			static_cast<std::atomic<int32_t>*>(pTimer->data)->store(iChecked, std::memory_order_release);
			uv_close(reinterpret_cast<uv_handle_t*>(pTimer), [](uv_handle_t* pHandle)
			{
				delete reinterpret_cast<uv_timer_t*>(pHandle);
			});
		}, 1, 0);
	}, &iSyncGuard);
	while (iSyncGuard.load(std::memory_order_acquire) == 0)
	{
		co_await OI::Coro::yield(10);
	}
	if (iSyncGuard.load() != 1)
	{
		++iBad;
	}
	if (!StatusIs(::OI::Redis::CallRedisSync({"PING"}), "PONG"))
	{
		++iBad;
	}
	co_return iBad;
}

static OI::Coro::TaskResult<bool> AwaitSingleUse(std::shared_ptr<OI::Redis::RedisCall> pCall)
{
	try
	{
		if (IntIs(co_await *pCall, 1))
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
static OI::Coro::TaskResult<int> CheckSingleUse()
{
	int iBad = 0;
	std::vector<std::string> vSingleUseKeys{"DEL", "oi-single-use", "oi-concurrent-use"};
	if (!(co_await OI::Redis::CallRedis(std::move(vSingleUseKeys))))
	{
		++iBad;
	}
	auto stSingleUse = OI::Redis::CallRedis({"INCR", "oi-single-use"});
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
	if (!BulkIs(co_await OI::Redis::CallRedis({"GET", "oi-single-use"}), "1", 1))
	{
		++iBad;
	}
	auto pSharedCall = std::make_shared<OI::Redis::RedisCall>(OI::Redis::CallRedis({"INCR", "oi-concurrent-use"}));
	auto pReadyCount = std::make_shared<std::atomic<int>>(0);
	auto fnContend = [pSharedCall, pReadyCount](OI::Coro::Token) -> OI::Coro::TaskResult<bool>
	{
		pReadyCount->fetch_add(1, std::memory_order_acq_rel);
		// 两个独立任务都就绪后再争抢；单线程等待时让出调度。
		while (pReadyCount->load(std::memory_order_acquire) < 2)
		{
			co_await OI::Coro::yield(0);
		}
		co_return co_await AwaitSingleUse(pSharedCall);
	};
	auto stFirst = OI::Coro::StartTask(fnContend);
	auto stSecond = OI::Coro::StartTask(fnContend);
	const int iAccepted = (co_await stFirst ? 1 : 0) + (co_await stSecond ? 1 : 0);
	if (iAccepted != 1 || !BulkIs(co_await OI::Redis::CallRedis({"GET", "oi-concurrent-use"}), "1", 1))
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

static OI::Coro::TaskResult<int> CheckCommands()
{
	using namespace OI::Redis;
	int iBad = 0;
	const std::vector<std::string> vKeys{"rk", "rbin", "rk-missing", "rk-exp", "rh", "rl", "rs", "rk-count", "rz", "rproto", "rproto-list", "rproto-z", "rproto-h", "rproto-bad"};
	for (const auto& sKey : vKeys)
	{
		if (co_await RedisDel(sKey) != OI_OK)
		{
			LOG_TRACE("redis command check failed line=%d", __LINE__);
			++iBad;
		}
	}
	std::string sValue;
	const std::string sBinary("a\0b", 3);
	const std::string sBinaryKey("r\0key", 5);
	if (co_await RedisSet("rk", "hello") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGet("rk", sValue) != OI_OK || sValue != "hello")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSet(sBinaryKey, sBinary) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGet(sBinaryKey, sValue) != OI_OK || sValue != sBinary)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisDel(sBinaryKey) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	sValue = "unchanged";
	if (co_await RedisGet("rk-missing", sValue) != OI_ERR_NOT_FOUND || sValue != "unchanged")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSet("rbin", "") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGet("rbin", sValue) != OI_OK || !sValue.empty())
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	int64_t iCount = -1;
	if (co_await RedisHSet("rh", "f", "v", iCount) != OI_OK || iCount != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHGet("rh", "f", sValue) != OI_OK || sValue != "v")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	sValue = "unchanged";
	if (co_await RedisGet("rh", sValue) != OI_ERR_REDIS_WRONGTYPE || sValue != "unchanged")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPush("rl", "a") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPush("rl", "b") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<std::string> vStrings;
	if (co_await RedisLRange("rl", 0, -1, vStrings) != OI_OK || (vStrings.size() != 2 || vStrings[0] != "a" || vStrings[1] != "b"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	bool bMember = false;
	if (co_await RedisSAdd("rs", "m") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSIsMember("rs", "m", bMember) != OI_OK || !bMember)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	for (int64_t iExpected = 1; iExpected <= 20; ++iExpected)
	{
		if (co_await RedisIncr("rk-count", iCount) != OI_OK || iCount != iExpected)
		{
			LOG_TRACE("redis command check failed line=%d", __LINE__);
			++iBad;
		}
	}
	OI::RpcData::RegInstance stRecord;
	stRecord.sName = sBinary;
	stRecord.iId = 123;
	stRecord.sIp = "127.0.0.1";
	OI::RpcData::RegInstance stRead;
	if (co_await RedisSet("rproto", stRecord) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGet("rproto", stRead) != OI_OK || stRead.sName != sBinary || stRead.iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	stRead.iId = 999;
	if (co_await RedisSet("rproto-bad", std::string(1, '\xff')) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGet("rproto-bad", stRead) != OI_ERR_DECODE || stRead.iId != 999)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHSet("rproto-h", "f", stRecord) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::map<std::string, OI::RpcData::RegInstance> mRecords;
	if (co_await RedisHGetAll("rproto-h", mRecords) != OI_OK || mRecords.size() != 1 || mRecords.at("f").iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPush("rproto-list", stRecord) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<OI::RpcData::RegInstance> vRecords;
	if (co_await RedisLRange("rproto-list", 0, -1, vRecords) != OI_OK || vRecords.size() != 1 || vRecords[0].iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPush("rproto-list", std::string(1, '\xff')) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLRange("rproto-list", 0, -1, vRecords) != OI_ERR_DECODE || vRecords.size() != 1 || vRecords[0].iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("rz", 1.25, "a", iCount) != OI_OK || iCount != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("rz", 2.5, "b") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("rz", 3.75, "a", iCount) != OI_OK || iCount != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRange("rz", 0, -1, vStrings) != OI_OK || (vStrings.size() != 2 || vStrings[0] != "b" || vStrings[1] != "a"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRevRange("rz", 0, -1, vStrings) != OI_OK || (vStrings.size() != 2 || vStrings[0] != "a" || vStrings[1] != "b"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<RedisScored<std::string>> vScores;
	if (co_await RedisZRevRangeWithScores("rz", 0, -1, vScores) != OI_OK || vScores.size() != 2 || vScores[0].stMember != "a" || vScores[0].dScore != 3.75)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	double dScore = -1;
	if (co_await RedisZScore("rz", "a", dScore) != OI_OK || dScore != 3.75)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRank("rz", "a", iCount) != OI_OK || iCount != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZScore("rz", "missing", dScore) != OI_ERR_NOT_FOUND || dScore != 3.75)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("rz", std::numeric_limits<double>::quiet_NaN(), "bad") != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("rproto-z", 9.5, stRecord) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<RedisScored<OI::RpcData::RegInstance>> vRecordScores;
	if (co_await RedisZRangeWithScores("rproto-z", 0, -1, vRecordScores) != OI_OK || vRecordScores.size() != 1 || vRecordScores[0].stMember.iId != 123 || vRecordScores[0].dScore != 9.5)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	// 单 key 空集合是成功空结果；不存在的标量是 NOT_FOUND，二者不能混淆。
	if (co_await RedisZRange("rk-missing", 0, -1, vStrings) != OI_OK || !vStrings.empty())
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	auto stSharedRead = RedisGet("rproto", stRead);
	if (co_await stSharedRead != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await stSharedRead != OI_OK || stRead.iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	LOG_TRACE("redis typed commands string rpcdata zset bad=%d", iBad);
	co_return iBad;
}

static OI::Coro::TaskResult<int> CheckCollectionCommands()
{
	using namespace OI::Redis;
	int iBad = 0;
	int64_t iNumber = -99;
	std::string sOutput;
	if (co_await RedisExists("rk", iNumber) != OI_OK || iNumber != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisExists("rk-missing", iNumber) != OI_OK || iNumber != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisTtl("rk", iNumber) != OI_OK || iNumber != -1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisPTtl("rk-missing", iNumber) != OI_OK || iNumber != -2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisDecr("rk-count", iNumber) != OI_OK || iNumber != 19)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHSet("rh", "f", "updated", iNumber) != OI_OK || iNumber != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHLen("rh", iNumber) != OI_OK || iNumber != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHExists("rh", "f", iNumber) != OI_OK || iNumber != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHDel("rh", "f", iNumber) != OI_OK || iNumber != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	sOutput = "unchanged";
	if (co_await RedisHGet("rh", "f", sOutput) != OI_ERR_NOT_FOUND || sOutput != "unchanged")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLSet("rl", -1, "c") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLLen("rl", iNumber) != OI_OK || iNumber != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPop("rl", sOutput) != OI_OK || sOutput != "a")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPop("rl", sOutput) != OI_OK || sOutput != "c")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPop("rl", sOutput) != OI_ERR_NOT_FOUND || sOutput != "c")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<std::string> vStrings;
	if (co_await RedisSAdd("rs", "m", iNumber) != OI_OK || iNumber != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSCard("rs", iNumber) != OI_OK || iNumber != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSMembers("rs", vStrings) != OI_OK || (vStrings.size() != 1 || vStrings[0] != "m"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	bool bMember = true;
	if (co_await RedisSIsMember("rs", "absent", bMember) != OI_OK || bMember)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSRem("rs", "m", iNumber) != OI_OK || iNumber != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	OI::RpcData::RegInstance stRecord;
	if (co_await RedisGet("rproto", stRecord) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	const std::string sKey = "rproto-collections";
	if (co_await RedisDel(sKey) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSAdd(sKey, stRecord) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<OI::RpcData::RegInstance> vRecords;
	if (co_await RedisSMembers(sKey, vRecords) != OI_OK || vRecords.size() != 1 || vRecords[0].iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSIsMember(sKey, stRecord, bMember) != OI_OK || !bMember)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSRem(sKey, stRecord) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPush(sKey, stRecord, iNumber) != OI_OK || iNumber != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLSet(sKey, 0, stRecord) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLTrim(sKey, 0, 0) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	OI::RpcData::RegInstance stOutput;
	if (co_await RedisRPop(sKey, stOutput) != OI_OK || stOutput.iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHGet("rproto-h", "f", stOutput) != OI_OK || stOutput.iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::map<std::string, std::string> mStrings;
	if (co_await RedisHSet("rh", "f", "v") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHGetAll("rh", mStrings) != OI_OK || (mStrings.size() != 1 || mStrings.at("f") != "v"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZCard("rz", iNumber) != OI_OK || iNumber != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRevRank("rz", "a", iNumber) != OI_OK || iNumber != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRank("rz", "missing", iNumber) != OI_ERR_NOT_FOUND || iNumber != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRange("rproto-z", -1, -1, vRecords) != OI_OK || vRecords.size() != 1 || vRecords[0].iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRevRange("rproto-z", 0, -1, vRecords) != OI_OK || vRecords.size() != 1 || vRecords[0].iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<RedisScored<OI::RpcData::RegInstance>> vScores;
	if (co_await RedisZRevRangeWithScores("rproto-z", 0, -1, vScores) != OI_OK || vScores.size() != 1 || vScores[0].dScore != 9.5)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRem("rproto-z", stRecord, iNumber) != OI_OK || iNumber != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRem("rz", "a") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisDel(sKey, iNumber) != OI_OK || iNumber != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	LOG_TRACE("redis typed collections string rpcdata bad=%d", iBad);
	co_return iBad;
}

static OI::Coro::TaskResult<int> CheckExpandedCommands()
{
	using namespace OI::Redis;
	int iBad = 0;
	int64_t iCount = 0;
	double dValue = 0;
	bool bValue = false;
	std::string sValue;
	const std::string sBinary("a\0b", 3);
	const std::vector<std::string> vEmpty;
	const std::vector<std::string> vPair{"a", "b"};
	const std::vector<std::string> vMembers{"a", "missing", "b"};
	const std::map<std::string, std::string> mEmpty;
	const std::map<std::string, std::string> mFields{{"a", sBinary}, {"b", "value"}};
	std::vector<std::string> vValues;
	std::vector<std::optional<std::string>> vOptional;
	std::vector<std::optional<double>> vOptionalScores;
	std::vector<int64_t> vMembership;
	std::vector<RedisScored<std::string>> vScores;
	const std::vector<RedisScored<std::string>> vScored{{"a", 1.0}, {"b", 2.0}, {"c", 3.0}};
	const std::vector<RedisScored<std::string>> vEmptyScored;
	const std::vector<std::string> vKeys{"expand:string", "expand:count", "expand:float", "expand:hash", "expand:list", "expand:set", "expand:zset", "expand:lex", "expand:rpc"};
	for (const auto& sKey : vKeys)
	{
		if (co_await RedisDel(sKey) != OI_OK)
		{
			LOG_TRACE("redis command check failed line=%d", __LINE__);
			++iBad;
		}
	}
	vValues = vPair;
	if (co_await RedisLPop("expand:list", 2, vValues) != OI_ERR_NOT_FOUND || vValues.size() != 2 || vValues[0] != "a" || vValues[1] != "b")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPop("expand:list", 2, vValues) != OI_ERR_NOT_FOUND || vValues.size() != 2 || vValues[0] != "a" || vValues[1] != "b")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSPop("expand:set", 2, vValues) != OI_OK || !vValues.empty())
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPush("expand:list", vEmpty) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPush("expand:list", vEmpty) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPushX("expand:list", vEmpty) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPushX("expand:list", vEmpty) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSAdd("expand:set", vEmpty) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSRem("expand:set", vEmpty) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRem("expand:zset", vEmpty) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHDel("expand:hash", vEmpty) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSetNx("expand:string", sBinary, bValue) != OI_OK || !bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSetNx("expand:string", "x", bValue) != OI_OK || bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisType("expand:string", sValue) != OI_OK || sValue != "string")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisStrLen("expand:string", iCount) != OI_OK || iCount != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisAppend("expand:string", "z", iCount) != OI_OK || iCount != 4)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGetRange("expand:string", 0, 2, sValue) != OI_OK || sValue != sBinary)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSetRange("expand:string", 3, "q", iCount) != OI_OK || iCount != 4)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGetSet("expand:string", sBinary, sValue) != OI_OK || sValue != sBinary + "q")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGetEx("expand:string", OI::Timer::Seconds(60), sValue) != OI_OK || sValue != sBinary)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisPersist("expand:string", bValue) != OI_OK || !bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSet("expand:string", sBinary, OI::Timer::Seconds(60)) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGetPersist("expand:string", sValue) != OI_OK || sValue != sBinary)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisExpireAt("expand:string", 4102444800LL, bValue) != OI_OK || !bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisPExpireAt("expand:string", 4102444800000LL, bValue) != OI_OK || !bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisGetDel("expand:string", sValue) != OI_OK || sValue != sBinary)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	sValue = "unchanged";
	if (co_await RedisGetDel("expand:string", sValue) != OI_ERR_NOT_FOUND || sValue != "unchanged")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisUnlink("expand:string", iCount) != OI_OK || iCount != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisIncrBy("expand:count", 5, iCount) != OI_OK || iCount != 5)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisDecrBy("expand:count", 2, iCount) != OI_OK || iCount != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisIncrByFloat("expand:float", 1.25, dValue) != OI_OK || dValue != 1.25)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHSet("expand:hash", mEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHSet("expand:hash", mFields, iCount) != OI_OK || iCount != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHSet("expand:hash", mFields) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHSetNx("expand:hash", "a", "x", bValue) != OI_OK || bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHSetNx("expand:hash", "new", "x", bValue) != OI_OK || !bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHKeys("expand:hash", vValues) != OI_OK || vValues.size() != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHVals("expand:hash", vValues) != OI_OK || vValues.size() != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHStrLen("expand:hash", "a", iCount) != OI_OK || iCount != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHIncrBy("expand:hash", "integer", 3, iCount) != OI_OK || iCount != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHIncrByFloat("expand:hash", "float", 1.5, dValue) != OI_OK || dValue != 1.5)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHMGet("expand:hash", vMembers, vOptional) != OI_OK || vOptional.size() != 3 || !vOptional[0] || *vOptional[0] != sBinary || vOptional[1] || !vOptional[2] || *vOptional[2] != "value")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHMGet("expand:hash", vEmpty, vOptional) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	RedisScanPage<std::map<std::string, std::string>> stHashPage;
	std::map<std::string, std::string> mScanned;
	uint64_t iCursor = 0;
	do
	{
		if (co_await RedisHScan("expand:hash", iCursor, stHashPage, 1) != OI_OK)
		{
			LOG_TRACE("redis command check failed line=%d", __LINE__);
			++iBad;
			break;
		}
		mScanned.insert(stHashPage.stValues.begin(), stHashPage.stValues.end());
		iCursor = stHashPage.iCursor;
	} while (iCursor != 0);
	if (mScanned.size() != 5 || mScanned["a"] != sBinary || mScanned["b"] != "value")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHScan("expand:hash", 0, stHashPage, 100, "no-match-*") != OI_OK || stHashPage.iCursor != 0 || !stHashPage.stValues.empty())
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHDel("expand:hash", vEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHDel("expand:hash", vPair, iCount) != OI_OK || iCount != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPush("expand:list", vEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPush("expand:list", vEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPushX("expand:list", vEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPushX("expand:list", vEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPushX("expand:list", "x", iCount) != OI_OK || iCount != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPushX("expand:list", "x", iCount) != OI_OK || iCount != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPush("expand:list", vPair, iCount) != OI_OK || iCount != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPush("expand:list", vPair, iCount) != OI_OK || iCount != 4)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPushX("expand:list", vPair, iCount) != OI_OK || iCount != 6)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPushX("expand:list", vPair, iCount) != OI_OK || iCount != 8)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPushX("expand:list", "x", iCount) != OI_OK || iCount != 9)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPushX("expand:list", "y", iCount) != OI_OK || iCount != 10)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLIndex("expand:list", 0, sValue) != OI_OK || sValue != "x")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPos("expand:list", "x", iCount) != OI_OK || iCount != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLInsertBefore("expand:list", "x", "before", iCount) != OI_OK || iCount != 11)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLInsertAfter("expand:list", "y", "after", iCount) != OI_OK || iCount != 12)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLRem("expand:list", 0, "x", iCount) != OI_OK || iCount != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisLPop("expand:list", 2, vValues) != OI_OK || vValues.size() != 2 || vValues[0] != "before")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisRPop("expand:list", 2, vValues) != OI_OK || vValues.size() != 2 || vValues[0] != "after" || vValues[1] != "y")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSAdd("expand:set", vEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSRem("expand:set", vEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSMIsMember("expand:set", vEmpty, vMembership) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSAdd("expand:set", vPair, iCount) != OI_OK || iCount != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSMIsMember("expand:set", vMembers, vMembership) != OI_OK || vMembership.size() != 3 || !vMembership[0] || vMembership[1] || !vMembership[2])
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSRandMember("expand:set", sValue) != OI_OK || (sValue != "a" && sValue != "b"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSRandMember("expand:set", 2, vValues) != OI_OK || vValues.size() != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	RedisScanPage<std::vector<std::string>> stSetPage;
	std::set<std::string> vSetScanned;
	iCursor = 0;
	do
	{
		if (co_await RedisSScan("expand:set", iCursor, stSetPage, 1) != OI_OK)
		{
			LOG_TRACE("redis command check failed line=%d", __LINE__);
			++iBad;
			break;
		}
		vSetScanned.insert(stSetPage.stValues.begin(), stSetPage.stValues.end());
		iCursor = stSetPage.iCursor;
	} while (iCursor != 0);
	if (vSetScanned.size() != 2 || vSetScanned.count("a") != 1 || vSetScanned.count("b") != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSScan("expand:set", 0, stSetPage, 100, "no-match-*") != OI_OK || stSetPage.iCursor != 0 || !stSetPage.stValues.empty())
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSPop("expand:set", sValue) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSPop("expand:set", 2, vValues) != OI_OK || vValues.size() != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSAdd("expand:set", vPair, iCount) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisSRem("expand:set", vPair, iCount) != OI_OK || iCount != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("expand:zset", vEmptyScored, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("expand:zset", vEmptyScored) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZMScore("expand:zset", vEmpty, vOptionalScores) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("expand:zset", vScored, iCount) != OI_OK || iCount != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("expand:zset", vScored) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAddNx("expand:zset", 4, "a", iCount) != OI_OK || iCount != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAddXx("expand:zset", 4, "missing", iCount) != OI_OK || iCount != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAddChanged("expand:zset", 4, "c", iCount) != OI_OK || iCount != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZIncrBy("expand:zset", 1, "b", dValue) != OI_OK || dValue != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZMScore("expand:zset", vMembers, vOptionalScores) != OI_OK || vOptionalScores.size() != 3 || !vOptionalScores[0] || *vOptionalScores[0] != 1 || vOptionalScores[1] || !vOptionalScores[2] || *vOptionalScores[2] != 3)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZCount("expand:zset", "1", "3", iCount) != OI_OK || iCount != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRangeByScore("expand:zset", "-inf", "+inf", vValues) != OI_OK || vValues.size() != 3 || vValues[0] != "a" || vValues[2] != "c")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRevRangeByScore("expand:zset", "+inf", "-inf", vValues) != OI_OK || vValues.size() != 3 || vValues[0] != "c" || vValues[2] != "a")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRangeByScore("expand:zset", "(1", "4", vValues, 1, 1) != OI_OK || vValues.size() != 1 || vValues[0] != "c")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRevRangeByScore("expand:zset", "(4", "-inf", vValues, 0, 1) != OI_OK || vValues.size() != 1 || vValues[0] != "b")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRangeByScoreWithScores("expand:zset", "-inf", "+inf", vScores) != OI_OK || vScores.size() != 3 || vScores[0].dScore != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRevRangeByScoreWithScores("expand:zset", "+inf", "-inf", vScores) != OI_OK || vScores.size() != 3 || vScores[0].dScore != 4)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	RedisScanPage<std::vector<RedisScored<std::string>>> stZPage;
	std::map<std::string, double> mScores;
	iCursor = 0;
	do
	{
		if (co_await RedisZScan("expand:zset", iCursor, stZPage, 1) != OI_OK)
		{
			LOG_TRACE("redis command check failed line=%d", __LINE__);
			++iBad;
			break;
		}
		for (const auto& stScore : stZPage.stValues)
		{
			mScores[stScore.stMember] = stScore.dScore;
		}
		iCursor = stZPage.iCursor;
	} while (iCursor != 0);
	if (mScores.size() != 3 || mScores["a"] != 1 || mScores["c"] != 4)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZScan("expand:zset", 0, stZPage, 100, "no-match-*") != OI_OK || stZPage.iCursor != 0 || !stZPage.stValues.empty())
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZPopMin("expand:zset", vScores) != OI_OK || vScores.size() != 1 || vScores[0].stMember != "a")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZPopMax("expand:zset", vScores) != OI_OK || vScores.size() != 1 || vScores[0].stMember != "c")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRemRangeByScore("expand:zset", "-inf", "+inf", iCount) != OI_OK || iCount != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("expand:zset", vScored, iCount) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRemRangeByRank("expand:zset", 0, 0, iCount) != OI_OK || iCount != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRem("expand:zset", vEmpty, iCount) != OI_ERR_INVALID_ARGUMENT)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRem("expand:zset", vMembers, iCount) != OI_OK || iCount != 1)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRem("expand:zset", vMembers, iCount) != OI_OK || iCount != 0)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("expand:lex", 0, "a") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZAdd("expand:lex", 0, "b") != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZLexCount("expand:lex", "-", "+", iCount) != OI_OK || iCount != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRangeByLex("expand:lex", "-", "+", vValues) != OI_OK || vValues.size() != 2 || vValues[0] != "a")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRangeByLex("expand:lex", "(a", "+", vValues) != OI_OK || vValues.size() != 1 || vValues[0] != "b")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRevRangeByLex("expand:lex", "+", "-", vValues) != OI_OK || vValues.size() != 2 || vValues[0] != "b")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisZRemRangeByLex("expand:lex", "-", "+", iCount) != OI_OK || iCount != 2)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	OI::RpcData::RegInstance stRecord;
	stRecord.iId = 321;
	stRecord.sName = sBinary;
	const std::map<std::string, OI::RpcData::RegInstance> mRecords{{"record", stRecord}};
	const std::vector<std::string> vRecordFields{"missing", "record"};
	std::vector<std::optional<OI::RpcData::RegInstance>> vOptionalRecords;
	if (co_await RedisHSet("expand:rpc", mRecords, iCount) != OI_OK)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await RedisHMGet("expand:rpc", vRecordFields, vOptionalRecords) != OI_OK || vOptionalRecords.size() != 2 || vOptionalRecords[0] || !vOptionalRecords[1] || vOptionalRecords[1]->iId != 321 || vOptionalRecords[1]->sName != sBinary)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	for (const auto& sKey : vKeys)
	{
		if (co_await RedisDel(sKey) != OI_OK)
		{
			LOG_TRACE("redis command check failed line=%d", __LINE__);
			++iBad;
		}
	}
	LOG_TRACE("redis expanded five types bad=%d", iBad);
	co_return iBad;
}

static OI::Coro::TaskResult<int> CheckTypedNativeCommands()
{
	using namespace OI::Redis;
	int iBad = 0;
	std::string sOutput;
	if (co_await CallRedis({"PING"}, sOutput) != OI_OK || sOutput != "PONG")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	const std::string sBinary("lua\0binary", 10);
	if (co_await CallRedis({"ECHO", sBinary}, sOutput) != OI_OK || sOutput != sBinary)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await CallRedis({"EVAL", "return ARGV[1]", "0", sBinary}, sOutput) != OI_OK || sOutput != sBinary)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	int64_t iNumber = -1;
	if (co_await CallRedis({"EVAL", "return 42", "0"}, iNumber) != OI_OK || iNumber != 42)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	bool bValue = false;
	if (co_await CallRedis({"EVAL", "return 1", "0"}, bValue) != OI_OK || !bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await CallRedis({"EVAL", "return false", "0"}, bValue) != OI_ERR_NOT_FOUND || !bValue)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	double dValue = 0;
	if (co_await CallRedis({"EVAL", "return '3.75'", "0"}, dValue) != OI_OK || dValue != 3.75)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<int64_t> vNumbers;
	if (co_await CallRedis({"EVAL", "return {1,2,3}", "0"}, vNumbers) != OI_OK || (vNumbers.size() != 3 || vNumbers[0] != 1 || vNumbers[1] != 2 || vNumbers[2] != 3))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<std::string> vStrings;
	if (co_await CallRedis({"EVAL", "return {'a','b'}", "0"}, vStrings) != OI_OK || (vStrings.size() != 2 || vStrings[0] != "a" || vStrings[1] != "b"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await CallRedis({"EVAL", "return {'new',42}", "0"}, vStrings) != OI_ERR_PROTOCOL || (vStrings.size() != 2 || vStrings[0] != "a" || vStrings[1] != "b"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	OI::RpcData::RegInstance stOutput;
	// Lua 显式声明且仅使用一个 key；返回的原始字节走同一 rpcdata 模板解码。
	if (co_await CallRedis({"EVAL", "return redis.call('GET', KEYS[1])", "1", "rproto"}, stOutput) != OI_OK || stOutput.iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	std::vector<OI::RpcData::RegInstance> vRecords;
	if (co_await CallRedis({"EVAL", "local v=redis.call('GET', KEYS[1]); return {v,v}", "1", "rproto"}, vRecords) != OI_OK || vRecords.size() != 2 || vRecords[1].iId != 123)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	sOutput = "unchanged";
	if (co_await CallRedis({"EVAL", "return redis.error_reply('test error')", "0"}, sOutput) != OI_ERR_REDIS || sOutput != "unchanged")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await CallRedis({"EVAL", "return false", "0"}, sOutput) != OI_ERR_NOT_FOUND || sOutput != "unchanged")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await CallRedis(std::vector<std::string>{}, sOutput) != OI_ERR_INVALID_ARGUMENT || sOutput != "unchanged")
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	uint8_t iSmall = 7;
	if (co_await CallRedis({"EVAL", "return 256", "0"}, iSmall) != OI_ERR_PROTOCOL || iSmall != 7)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	if (co_await CallRedis({"PING"}, iNumber) != OI_ERR_PROTOCOL || iNumber != 42)
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	auto stRaw = co_await CallRedis({"PING"});
	if (stRaw.GetRet() != OI_OK || !StatusIs(stRaw, "PONG"))
	{
		LOG_TRACE("redis command check failed line=%d", __LINE__);
		++iBad;
	}
	LOG_TRACE("redis typed native ping lua scalar array rpcdata bad=%d", iBad);
	co_return iBad;
}

static OI::Coro::TaskResult<int> CheckExpiry()
{
	int64_t iChanged = 0;
	int64_t iRemaining = 0;
	const int32_t iSetRet = co_await OI::Redis::RedisSet("rk-exp", "1");
	const int32_t iSecondsRet = co_await OI::Redis::RedisExpire("rk-exp", OI::Timer::Seconds(60), iChanged);
	const bool bSecondsChanged = iSecondsRet == OI_OK && iChanged == 1;
	const int32_t iMillisecondsRet = co_await OI::Redis::RedisExpire("rk-exp", OI::Timer::Milliseconds(750), iChanged);
	const bool bMillisecondsChanged = iMillisecondsRet == OI_OK && iChanged == 1;
	const int32_t iTimeToLiveRet = co_await OI::Redis::RedisPTtl("rk-exp", iRemaining);
	const bool bGood = iSetRet == OI_OK && bSecondsChanged && bMillisecondsChanged &&
	                   iTimeToLiveRet == OI_OK && iRemaining > 0 && iRemaining <= 750;
	LOG_TRACE("redis millisecond expiry bad=%d", bGood ? 0 : 1);
	co_return bGood ? 0 : 1;
}

// runner 将普通 Redis 用例配置成单条上游，两条下游必须互不污染。
static OI::Coro::TaskResult<int> CheckScriptIsolation()
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
			co_await OI::Coro::yield(1);
		}
		if (stScriptPeer.Replies() != iExpected || stScriptPeer.Bad() != 0)
		{
			++iBad;
		}
		if (!(co_await OI::Redis::RedisClient::GetInstance().StartClient()))
		{
			++iBad;
		}
		if (!StatusIs(co_await OI::Redis::CallRedis({"PING"}), "PONG"))
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
static OI::Coro::TaskResult<int> CheckDeniedCommands()
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
			co_await OI::Coro::yield(50);
		}
		if (!StatusIs(co_await ::OI::Redis::CallRedis({"PING"}), "PONG"))
		{
			++iBad;
		}
	}
	co_return iBad;
}

// 由既有测试驱动仅重启本轮拥有的 Redis；模块继续连接 redisserver。
static OI::Coro::TaskResult<int> CheckUpstreamDisconnect()
{
	std::vector<std::string> vOutput{"unchanged"};
	auto stPending = OI::Redis::CallRedis({"BLPOP", "oi:disconnect:empty", "0"}, vOutput);
	{
		std::ofstream stRequest("../redis-disconnect.request");
		stRequest << "restart";
	}
	const int32_t iRet = co_await stPending;
	int iBad = iRet != OI_ERR_REDIS || vOutput != std::vector<std::string>{"unchanged"} ? 1 : 0;
	const int64_t iDeadline = OI::Timer::SteadyMilliseconds() + 10000;
	bool bAcknowledged = false;
	while (OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		std::ifstream stAck("../redis-disconnect.ack");
		if (stAck.good())
		{
			bAcknowledged = true;
			break;
		}
		co_await OI::Coro::yield(OI::Timer::Milliseconds(20));
	}
	bool bRecovered = false;
	while (bAcknowledged && OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		std::string sPong;
		const int32_t iPingRet = co_await OI::Redis::CallRedis({"PING"}, sPong);
		if (iPingRet == OI_OK && sPong == "PONG")
		{
			bRecovered = true;
			break;
		}
		co_await OI::Coro::yield(OI::Timer::Milliseconds(100));
	}
	if (!bRecovered)
	{
		++iBad;
	}
	LOG_TRACE("redis upstream disconnect recovery bad=%d", iBad);
	co_return iBad;
}

// 临时配置限定客户端发送预算为 1MiB；真实发送拒绝后，后续回执仍应准确配对。
static OI::Coro::TaskResult<int> CheckSendBudget()
{
	int iBad = 0;
	int iRejected = 0;
	int iRecovered = 0;
	auto& stClient = OI::Redis::RedisClient::GetInstance();
	const sid_t iSid = stClient.ConnectedSid();
	const std::string sLarge(2 * 1024 * 1024, 'x');
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		const std::string sBefore = "before-" + std::to_string(iIndex);
		const std::string sAfter = "after-" + std::to_string(iIndex);
		// 保留一个先发请求，再拒绝大请求并发送小请求，检查 FIFO 回滚不串位。
		std::vector<std::string> vBefore{"ECHO", sBefore};
		std::vector<std::string> vLarge{"ECHO", sLarge};
		std::vector<std::string> vAfter{"ECHO", sAfter};
		auto stBefore = AwaitRedis(std::move(vBefore));
		const auto stRejected = co_await OI::Redis::CallRedis(vLarge);
		if (stRejected.GetRet() == OI_ERR_BUSY && stRejected.iStatus == OI::Redis::RedisStatus::Busy && !stRejected.bTimeout && !stRejected.pReply)
		{
			++iRejected;
		}
		std::string sOutput = "unchanged";
		const int32_t iRet = co_await OI::Redis::CallRedis(std::move(vLarge), sOutput);
		if (iRet == OI_ERR_BUSY && sOutput == "unchanged")
		{
			++iRejected;
		}
		const int32_t iAfterRet = co_await OI::Redis::CallRedis(std::move(vAfter), sOutput);
		if (iAfterRet == OI_OK && sOutput == sAfter)
		{
			++iRecovered;
		}
		const auto stFirst = co_await stBefore;
		if (stFirst && stFirst.pReply->type == REDIS_REPLY_STRING && stFirst.pReply->str &&
		    std::string(stFirst.pReply->str, stFirst.pReply->len) == sBefore)
		{
			++iRecovered;
		}
	}
	const size_t iPending = stClient.PendingCount();
	if (iRejected != 16 || iRecovered != 16 || iPending != 0 || iSid == INVALID_SID || stClient.ConnectedSid() != iSid)
	{
		++iBad;
	}
	LOG_TRACE("redis send budget rejected=%d recovered=%d pending=%zu bad=%d", iRejected, iRecovered, iPending, iBad);
	co_return iBad;
}

OI::Coro::Task RedisMgr::Check()
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
	m_iFailed += co_await CheckCollectionCommands();
	m_iFailed += co_await CheckTypedNativeCommands();
	m_iFailed += co_await CheckExpandedCommands();
	m_iFailed += co_await CheckExpiry();
	m_iFailed += co_await CheckScriptIsolation();
	m_iFailed += co_await CheckDeniedCommands();
	bool bSendBudget = false;
	TryGetConfig("Regression.redis_send_budget", bSendBudget);
	if (bSendBudget)
	{
		m_iFailed += co_await CheckSendBudget();
	}
	bool bDisconnectRegression = false;
	TryGetConfig("Regression.redis_upstream_disconnect", bDisconnectRegression);
	if (bDisconnectRegression)
	{
		m_iFailed += co_await CheckUpstreamDisconnect();
	}
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

void RedisMgr::SyncStress(int iIdLocal)
{
	int iTypedBad = 0;
	std::string sKey = "sync:" + std::to_string(iIdLocal);
	OI::RpcData::RegInstance stWritten;
	stWritten.iId = iIdLocal;
	stWritten.sName = "sync-rpcdata";
	OI::RpcData::RegInstance stRead;
	const std::string sObjectKey = sKey + ":object";
	if (OI::Redis::RedisSetSync(sObjectKey, stWritten) != OI_OK ||
	    OI::Redis::RedisGetSync(sObjectKey, stRead) != OI_OK || stRead.iId != iIdLocal || stRead.sName != stWritten.sName)
	{
		++iTypedBad;
	}
	std::string sPong;
	int64_t iLuaValue = 0;
	std::vector<int64_t> vLuaValues;
	if (OI::Redis::CallRedisSync({"PING"}, sPong) != OI_OK || sPong != "PONG" ||
	    OI::Redis::CallRedisSync({"EVAL", "return 42", "0"}, iLuaValue) != OI_OK || iLuaValue != 42 ||
	    OI::Redis::CallRedisSync({"EVAL", "return {1,2}", "0"}, vLuaValues) != OI_OK || vLuaValues != std::vector<int64_t>{1, 2} ||
	    OI::Redis::CallRedisSync({"EVAL", "return redis.call('GET', KEYS[1])", "1", sObjectKey}, stRead) != OI_OK || stRead.iId != iIdLocal)
	{
		++iTypedBad;
	}
	const std::string sHashKey = sObjectKey + ":hash";
	const std::string sListKey = sObjectKey + ":list";
	const std::string sSetKey = sObjectKey + ":set";
	const std::string sZsetKey = sObjectKey + ":zset";
	for (const auto& sCleanKey : {sHashKey, sListKey, sSetKey, sZsetKey})
	{
		if (OI::Redis::RedisDelSync(sCleanKey) != OI_OK)
		{
			++iTypedBad;
		}
	}
	std::map<std::string, OI::RpcData::RegInstance> mRecords;
	std::vector<OI::RpcData::RegInstance> vRecords;
	std::vector<OI::Redis::RedisScored<OI::RpcData::RegInstance>> vScores;
	bool bMember = false;
	double dScore = 0;
	if (OI::Redis::RedisHSetSync(sHashKey, "f", stWritten) != OI_OK || OI::Redis::RedisHGetAllSync(sHashKey, mRecords) != OI_OK || mRecords.size() != 1 || mRecords.at("f").iId != iIdLocal)
	{
		++iTypedBad;
	}
	if (OI::Redis::RedisRPushSync(sListKey, stWritten) != OI_OK
		|| OI::Redis::RedisLRangeSync(sListKey, 0, -1, vRecords) != OI_OK
		|| vRecords.size() != 1
		|| vRecords[0].iId != iIdLocal)
	{
		++iTypedBad;
	}
	if (OI::Redis::RedisSAddSync(sSetKey, stWritten) != OI_OK
		|| OI::Redis::RedisSIsMemberSync(sSetKey, stWritten, bMember) != OI_OK
		|| !bMember
		|| OI::Redis::RedisSMembersSync(sSetKey, vRecords) != OI_OK
		|| vRecords.size() != 1
		|| vRecords[0].iId != iIdLocal)
	{
		++iTypedBad;
	}
	if (OI::Redis::RedisZAddSync(sZsetKey, 1.25, stWritten) != OI_OK
		|| OI::Redis::RedisZScoreSync(sZsetKey, stWritten, dScore) != OI_OK
		|| dScore != 1.25
		|| OI::Redis::RedisZRevRangeWithScoresSync(sZsetKey, 0, -1, vScores) != OI_OK
		|| vScores.size() != 1
		|| vScores[0].stMember.iId != iIdLocal
		|| vScores[0].dScore != 1.25)
	{
		++iTypedBad;
	}
	const std::vector<std::string> vFields{"missing", "f"};
	std::vector<std::optional<OI::RpcData::RegInstance>> vOptionalRecords;
	OI::RpcData::RegInstance stMissingRecord = stWritten;
	stMissingRecord.iId = iIdLocal + 100;
	std::vector<OI::RpcData::RegInstance> vScoreMembers{stMissingRecord, stWritten};
	std::vector<std::optional<double>> vOptionalScores;
	if (OI::Redis::RedisHMGetSync(sHashKey, vFields, vOptionalRecords) != OI_OK || vOptionalRecords.size() != 2 || vOptionalRecords[0] || !vOptionalRecords[1] || vOptionalRecords[1]->iId != iIdLocal)
	{
		++iTypedBad;
	}
	if (OI::Redis::RedisZMScoreSync(sZsetKey, vScoreMembers, vOptionalScores) != OI_OK || vOptionalScores.size() != 2 || vOptionalScores[0] || !vOptionalScores[1] || *vOptionalScores[1] != 1.25)
	{
		++iTypedBad;
	}
	OI::Redis::RedisScanPage<std::map<std::string, OI::RpcData::RegInstance>> stHashPage;
	OI::Redis::RedisScanPage<std::vector<OI::RpcData::RegInstance>> stSetPage;
	OI::Redis::RedisScanPage<std::vector<OI::Redis::RedisScored<OI::RpcData::RegInstance>>> stZPage;
	uint64_t iCursor = 0;
	bool bFound = false;
	do
	{
		if (OI::Redis::RedisHScanSync(sHashKey, iCursor, stHashPage, 1) != OI_OK)
		{
			++iTypedBad;
			break;
		}
		for (const auto& [sField, stValue] : stHashPage.stValues)
		{
			if (sField == "f" && stValue.iId == iIdLocal)
			{
				bFound = true;
			}
		}
		iCursor = stHashPage.iCursor;
	} while (iCursor != 0);
	if (!bFound)
	{
		++iTypedBad;
	}
	iCursor = 0;
	bFound = false;
	do
	{
		if (OI::Redis::RedisSScanSync(sSetKey, iCursor, stSetPage, 1) != OI_OK)
		{
			++iTypedBad;
			break;
		}
		for (const auto& stValue : stSetPage.stValues)
		{
			if (stValue.iId == iIdLocal)
			{
				bFound = true;
			}
		}
		iCursor = stSetPage.iCursor;
	} while (iCursor != 0);
	if (!bFound)
	{
		++iTypedBad;
	}
	iCursor = 0;
	bFound = false;
	do
	{
		if (OI::Redis::RedisZScanSync(sZsetKey, iCursor, stZPage, 1) != OI_OK)
		{
			++iTypedBad;
			break;
		}
		for (const auto& stValue : stZPage.stValues)
		{
			if (stValue.stMember.iId == iIdLocal && stValue.dScore == 1.25)
			{
				bFound = true;
			}
		}
		iCursor = stZPage.iCursor;
	} while (iCursor != 0);
	if (!bFound)
	{
		++iTypedBad;
	}

	const std::vector<OI::RpcData::RegInstance> vEmptyRecords;
	int64_t iRemoved = -1;
	const std::vector<std::string> vEmptyFields;
	if (OI::Redis::RedisLPushSync(sListKey, vEmptyRecords) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Redis::RedisRPushSync(sListKey, vEmptyRecords) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Redis::RedisLPushXSync(sListKey, vEmptyRecords) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Redis::RedisRPushXSync(sListKey, vEmptyRecords) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Redis::RedisSAddSync(sSetKey, vEmptyRecords) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Redis::RedisSRemSync(sSetKey, vEmptyRecords) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Redis::RedisZRemSync(sZsetKey, vEmptyRecords) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Redis::RedisHDelSync(sHashKey, vEmptyFields) != OI_ERR_INVALID_ARGUMENT)
	{
		++iTypedBad;
	}
	if (OI::Redis::RedisZRemSync(sZsetKey, vEmptyRecords, iRemoved) != OI_ERR_INVALID_ARGUMENT ||
	    OI::Redis::RedisZRemSync(sZsetKey, vScoreMembers, iRemoved) != OI_OK || iRemoved != 1 ||
	    OI::Redis::RedisZRemSync(sZsetKey, vScoreMembers, iRemoved) != OI_OK || iRemoved != 0)
	{
		++iTypedBad;
	}
	for (const auto& sCleanKey : {sObjectKey, sHashKey, sListKey, sSetKey, sZsetKey})
	{
		if (OI::Redis::RedisDelSync(sCleanKey) != OI_OK)
		{
			++iTypedBad;
		}
	}
	m_iBad.fetch_add(iTypedBad);
	LOG_TRACE("redis typed sync id=%d bad=%d", iIdLocal, iTypedBad);
	auto stEnd = m_stStart + std::chrono::seconds(m_iSeconds);
	while (std::chrono::steady_clock::now() < stEnd)
	{
		std::string sOutput;
		const int32_t iPut = OI::Redis::RedisSetSync(sKey, "s");
		const int32_t iGot = OI::Redis::RedisGetSync(sKey, sOutput);
		if (iPut != OI_OK || iGot != OI_OK || sOutput != "s")
		{
			m_iBad.fetch_add(1);
		}
		else
		{
			m_iOperations.fetch_add(2);
		}
	}
}

OI::Coro::Task RedisMgr::Stress(int iIdLocal)
{
	std::string sValueLocal(64, 'x');
	std::string sHash = "h:" + std::to_string(iIdLocal);
	std::string sList = "l:" + std::to_string(iIdLocal);
	auto stEnd = m_stStart + std::chrono::seconds(m_iSeconds);
	int iSequence = 0;
	int iKind = iIdLocal % 4;
	while (std::chrono::steady_clock::now() < stEnd)
	{
		std::string sKey = "k:" + std::to_string(iIdLocal) + ":" + std::to_string(iSequence % 256);
		std::string sMember = std::to_string(iSequence % 32);
		++iSequence;
		int64_t iNumber = 0;
		bool bOk = false;
		if (iKind == 0)
		{
			bOk = co_await OI::Redis::RedisSet(sKey, sValueLocal) == OI_OK;
		}
		else if (iKind == 1)
		{
			bOk = co_await OI::Redis::RedisHSet(sHash, sMember, sValueLocal, iNumber) == OI_OK && (iNumber == 0 || iNumber == 1);
		}
		else if (iKind == 2)
		{
			bOk = co_await OI::Redis::RedisLPush(sList, sMember) == OI_OK;
			if (bOk && iSequence % 16 == 0)
			{
				bOk = co_await OI::Redis::RedisLTrim(sList, 0, 15) == OI_OK;
			}
		}
		else
		{
			bOk = co_await OI::Redis::RedisIncr(sKey, iNumber) == OI_OK;
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
