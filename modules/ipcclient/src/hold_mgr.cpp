#include "hold_mgr.h"
#include "oi_http.h"
#include "oi_log_client.h"
#include "oi_log.h"
#include "oi_registry.h"
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#include "oi_rpcdata_jsonall.h"
#include <limits>
#include "oi_config.h"
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpcdata_holdreq.h"
#include "oi_rpcdata_holdres.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "oi_redis.h"
#include "oi_poll.h"
#include "oi_net_session.h"
#include "oi_timer_manager.h"
#include <uv.h>
#include "test_board.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

// 仅本回归持有的堆资源，不改变 Hold 的线上字段或协议号。
struct HoldRequestResource
{
	std::vector<char> vPayload = std::vector<char>(1024 * 1024, 'R');
	std::shared_ptr<std::atomic<int>> pDestroyed;
	explicit HoldRequestResource(std::shared_ptr<std::atomic<int>> pDestroyedParam)
	    : pDestroyed(std::move(pDestroyedParam))
	{
	}
	~HoldRequestResource()
	{
		pDestroyed->fetch_add(1, std::memory_order_relaxed);
	}
};

struct ResourceHoldRequest : OI::RpcData::HoldReq
{
	std::shared_ptr<HoldRequestResource> pResource;
	bool bThrow = false;
	OI::Base::BytesStream& serialize(OI::Base::BytesStream& stOutputParam) const override
	{
		if (!pResource || pResource->vPayload.size() != 1024 * 1024)
		{
			throw std::logic_error("request resource released before serialization");
		}
		if (bThrow)
		{
			throw std::runtime_error("request resource serialization failure");
		}
		return HoldReq::serialize(stOutputParam);
	}
};

namespace OI
{
namespace Rpc
{
// 复用生成的 Hold 配对，仅让测试附带一个不参与序列化的资源探针。
template <>
struct RpcProtoId<ResourceHoldRequest> : RpcProtoId<OI::RpcData::HoldReq>
{
};
}
}

static std::atomic<int> iReverseStarted{0};
static std::atomic<int> iReverseFinished{0};

static OI_RPC_HANDLER(Hold, HandleReverseHold)
{
	++iReverseStarted;
	co_await coro::yield(stRequest.iDelayMilliseconds);
	stResponse.iMark = stRequest.iMark;
	++iReverseFinished;
	co_return 0;
}

HoldMgr::HoldMgr()
    : m_bEnabled(false), m_iSessionId(0), m_iRedisTimeoutMilliseconds(25000)
{
}

void HoldMgr::Load()
{
	TryGetConfig("Test.hold", m_bEnabled);
	if (m_bEnabled)
	{
		OI_RPC_BIND(Hold, HandleReverseHold);
	}
	TryGetConfig("RedisClient.timeout_ms", m_iRedisTimeoutMilliseconds);
}

void HoldMgr::Start(sid_t iSessionIdParam)
{
	if (!m_bEnabled)
	{
		return;
	}
	m_iSessionId = iSessionIdParam;
	TestBoard::GetInstance().Arm();
	coro::StartTask([](coro::Token)
	{
		return HoldMgr::GetInstance().Pump();
	});
}

static long long CpuMs(std::clock_t iBeginParam)
{
	double dMilliseconds = 1000.0 * (double)(std::clock() - iBeginParam) / (double)CLOCKS_PER_SEC;
	if (dMilliseconds < 0)
	{
		dMilliseconds = 0;
	}
	return (long long)dMilliseconds;
}

static long long WallMs(int64_t iBeginMillisecondsParam)
{
	return ::OI::Timer::SteadyMilliseconds() - iBeginMillisecondsParam;
}

static bool Pong(const ::OI::Redis::RedisResult& stResultParam)
{
	return !stResultParam.bTimeout && stResultParam.pReply && stResultParam.pReply->type == REDIS_REPLY_STATUS && stResultParam.pReply->str && std::strcmp(stResultParam.pReply->str, "PONG") == 0;
}

enum class RaceOutcome
{
	Reply,
	Timeout,
	Bad,
};

static coro::TaskResult<RaceOutcome> RaceOne(int32_t iMarkParam, int32_t iDelayParam)
{
	::OI::RpcData::HoldReq stOne;
	stOne.iDelayMilliseconds = iDelayParam;
	stOne.iMark = iMarkParam;
	auto stResult = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stOne, ::OI::Timer::Seconds(1));
	if (stResult.GetRet() == 0)
	{
		co_return stResult.GetRes().iMark == iMarkParam ? RaceOutcome::Reply : RaceOutcome::Bad;
	}
	co_return stResult.GetRet() == OI_ERR_TIMEOUT ? RaceOutcome::Timeout : RaceOutcome::Bad;
}

// Run with the existing Test.hold switch in both thread modes.
template <typename T>
static int CheckJsonInteger()
{
	int iBad = 0;
	T stValue{};
	const Json::Value stMinimum = std::is_signed_v<T>
	                                  ? Json::Value(static_cast<Json::Int64>((std::numeric_limits<T>::min)()))
	                                  : Json::Value(Json::UInt64(0));
	const Json::Value stMaximum(static_cast<Json::UInt64>((std::numeric_limits<T>::max)()));
	if (!OI::Rpc::FromJsonValue(stMinimum, stValue) || stValue != (std::numeric_limits<T>::min)())
	{
		++iBad;
	}
	if (!OI::Rpc::FromJsonValue(stMaximum, stValue) || stValue != (std::numeric_limits<T>::max)())
	{
		++iBad;
	}
	std::vector<Json::Value> vInvalid{Json::Value(1.0), Json::Value(true), Json::Value("1"), Json::Value()};
	if constexpr (std::is_signed_v<T>)
	{
		vInvalid.emplace_back(static_cast<Json::UInt64>((std::numeric_limits<T>::max)()) + 1);
		if constexpr (sizeof(T) < sizeof(int64_t))
		{
			vInvalid.emplace_back(static_cast<Json::Int64>((std::numeric_limits<T>::min)()) - 1);
		}
	}
	else
	{
		vInvalid.emplace_back(-1);
		if constexpr (sizeof(T) < sizeof(uint64_t))
		{
			vInvalid.emplace_back(static_cast<Json::UInt64>((std::numeric_limits<T>::max)()) + 1);
		}
	}
	for (const auto& stInput : vInvalid)
	{
		stValue = 7;
		if (OI::Rpc::FromJsonValue(stInput, stValue) || stValue != 7)
		{
			++iBad;
		}
	}
	return iBad;
}

static int CheckJsonDecoding()
{
	int iBad = CheckJsonInteger<int8_t>() + CheckJsonInteger<uint8_t>() + CheckJsonInteger<int16_t>() + CheckJsonInteger<uint16_t>() + CheckJsonInteger<int32_t>() + CheckJsonInteger<uint32_t>() + CheckJsonInteger<int64_t>() + CheckJsonInteger<uint64_t>();
	Json::Value stInput(Json::objectValue);
	stInput[std::string(40, '0') + "1"] = 1;
	std::map<int32_t, int32_t> mSigned;
	if (!OI::Rpc::FromJsonValue(stInput, mSigned) || mSigned != std::map<int32_t, int32_t>{{1, 1}})
	{
		++iBad;
	}
	const std::vector<std::string> vInvalidKeys{"", "2147483648", "-2147483649", "1x", " 1", "+1", std::string("1\0x", 3)};
	for (const auto& sKey : vInvalidKeys)
	{
		stInput = Json::Value(Json::objectValue);
		stInput[sKey] = 2;
		if (OI::Rpc::FromJsonValue(stInput, mSigned) || mSigned != std::map<int32_t, int32_t>{{1, 1}})
		{
			++iBad;
		}
	}
	std::map<uint64_t, int32_t> mUnsigned{{3, 7}};
	for (const auto& sKey : std::vector<std::string>{"-1", "18446744073709551616", "1x"})
	{
		stInput = Json::Value(Json::objectValue);
		stInput[sKey] = 2;
		if (OI::Rpc::FromJsonValue(stInput, mUnsigned) || mUnsigned != std::map<uint64_t, int32_t>{{3, 7}})
		{
			++iBad;
		}
	}
	stInput = Json::Value(Json::objectValue);
	stInput["18446744073709551615"] = 2;
	if (!OI::Rpc::FromJsonValue(stInput, mUnsigned) || mUnsigned.count(UINT64_MAX) != 1)
	{
		++iBad;
	}
	std::vector<int8_t> vValues{7};
	Json::Value stArray(Json::arrayValue);
	stArray.append(1);
	stArray.append(128);
	if (OI::Rpc::FromJsonValue(stArray, vValues) || vValues != std::vector<int8_t>{7})
	{
		++iBad;
	}
	std::map<std::string, int8_t> mValues{{"old", 7}};
	stInput = Json::Value(Json::objectValue);
	stInput["a"] = 1;
	stInput["b"] = 128;
	if (OI::Rpc::FromJsonValue(stInput, mValues) || mValues != std::map<std::string, int8_t>{{"old", 7}})
	{
		++iBad;
	}
	OI::RpcData::JsonAll stOriginal;
	stOriginal.iInt8 = 7;
	stOriginal.sMessage = "unchanged";
	stOriginal.vNumbers = {3, 4};
	const std::string sBefore = stOriginal.ToJson();
	stInput = Json::Value(Json::objectValue);
	stInput["c"] = 1;
	stInput["u8"] = 256;
	if (stOriginal.FromJson(stInput) || stOriginal.ToJson() != sBefore)
	{
		++iBad;
	}
	stInput = Json::Value(Json::objectValue);
	stInput["c"] = 2;
	if (!stOriginal.FromJson(stInput) || stOriginal.iInt8 != 2 || stOriginal.sMessage != "unchanged" || stOriginal.vNumbers != std::vector<int32_t>{3, 4})
	{
		++iBad;
	}
	printf("hold json validation bad=%d\n", iBad);
	return iBad;
}

static int CheckBinaryBool()
{
	int iBad = 0;
	for (int iMode = 0; iMode < 2; ++iMode)
	{
		for (unsigned iByte = 0; iByte < 256; ++iByte)
		{
			const unsigned char iRaw = static_cast<unsigned char>(iByte);
			OI::Base::BytesStream stInput(reinterpret_cast<const char*>(&iRaw), 1);
			bool bValue = true;
			bool bRejected = false;
			try
			{
				if (iMode == 0)
				{
					stInput >> bValue;
				}
				else
				{
					stInput.ReadPb(1, bValue);
				}
			}
			catch (const OI::Base::ParserException&)
			{
				bRejected = true;
			}
			if (iByte <= 1)
			{
				if (bRejected || bValue != (iByte != 0) || stInput.GetCurPos() != 1)
				{
					++iBad;
				}
			}
			else if (!bRejected || !bValue || stInput.GetCurPos() != 0)
			{
				++iBad;
			}
		}
	}
	printf("hold binary bool bad=%d\n", iBad);
	return iBad;
}

// 在 poll 执行域里模拟旧建连完成/失败，不能取消当前代次或新增会话。
class ConnectGenerationProbe : public OI::Net::SessionManager
{
	int m_iFailures = 0;
	OI::Net::Session* CloneSession() override
	{
		return nullptr;
	}
	void OnAddSession(OI::Net::Session*) override
	{
	}
	void OnDelSession(const OI::Net::Session*) override
	{
	}
	void OnConnectFailed() override
	{
		++m_iFailures;
	}

  public:
	int Check()
	{
		iConnectGeneration = 2;
		bConnectPending = true;
		AbortSession(1);
		const auto iDescriptor = NetOs_CreateTcp(true);
		if (iDescriptor == INVALID_SOCKET_ID)
		{
			return 1;
		}
		CompleteConnect(iDescriptor, 1);
		int iBad = (!bConnectPending || m_iFailures != 0 || !mSessionMap.empty()) ? 1 : 0;
		AbortSession(2);
		AbortSession(2);
		if (bConnectPending || m_iFailures != 1)
		{
			++iBad;
		}
		return iBad;
	}
};

// 回归脚本按端口偏移改写配置，探针必须跟着配置走，不能写死端口。
static std::string IpcServerPort()
{
	std::string sPort = "17981";
	TryGetConfig("IpcClient.port", sPort);
	return sPort;
}

class ClientConnectProbe : public OI::Net::SessionManager
{
	class ProbeSession : public OI::Net::NetSession
	{
	  public:
		explicit ProbeSession(ClientConnectProbe* pOwnerParam)
		    : NetSession(pOwnerParam)
		{
		}
		void OnDataIn() override
		{
			stInputBuffer.Clear();
		}
		void OnDataOut() override
		{
		}
	};
	std::atomic<int> m_iConnections{0};
	bool m_bReconnectInCallback;
	std::optional<ConnectAwaiter> m_stRetry;
	OI::Net::Session* CloneSession() override
	{
		return new ProbeSession(this);
	}
	void OnAddSession(OI::Net::Session* pSessionParam) override
	{
		if (++m_iConnections == 1 && m_bReconnectInCallback)
		{
			CloseSession(pSessionParam->GetSessionID());
			DefaultInit("127.0.0.1", "0");
			m_stRetry.emplace(StartClient());
		}
	}
	void OnDelSession(const OI::Net::Session*) override
	{
	}

  public:
	explicit ClientConnectProbe(const std::string& sPortParam, bool bReconnectParam = false)
	    : m_bReconnectInCallback(bReconnectParam)
	{
		DefaultInit("127.0.0.1", sPortParam);
	}
	ConnectAwaiter& Retry()
	{
		return m_stRetry.value();
	}
	int Connections() const
	{
		return m_iConnections.load();
	}
};

static coro::TaskResult<int> AwaitClientConnect(ClientConnectProbe* pProbeParam)
{
	co_return (co_await pProbeParam->StartClient()) ? 0 : 1;
}

static coro::TaskResult<int> CheckClientConnect()
{
	static ClientConnectProbe stGood(IpcServerPort()), stRefused("0");
	int iBad = 0;
	static ClientConnectProbe stEager(IpcServerPort()), stDelayedAwait(IpcServerPort());
	stEager.StartClient();
	auto stConnecting = stDelayedAwait.StartClient();
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	while ((stEager.Connections() != 1 || stDelayedAwait.Connections() != 1) && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await coro::yield(1);
	}
	if (stEager.Connections() != 1 || stDelayedAwait.Connections() != 1 || !(co_await stConnecting))
	{
		++iBad;
	}
	if (!(co_await stConnecting))
	{
		++iBad;
	}
	stEager.CloseAllSessions();
	stDelayedAwait.CloseAllSessions();
	if (!(co_await stGood.StartClient()) || stGood.Connections() != 1)
	{
		++iBad;
	}
	if (!(co_await stGood.StartClient()) || stGood.Connections() != 1)
	{
		++iBad;
	}
	if (co_await stRefused.StartClient())
	{
		++iBad;
	}
	struct Result
	{
		std::atomic<bool> bDone{false};
		int iBad = 0;
		bool bReturned = false;
	};
	auto pResult = std::make_shared<Result>();
	OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgumentParam)
	{
		auto* pState = static_cast<Result*>(pArgumentParam);
		OI::Net::Poll::GetInstance().PostOnLoop([](void* pNestedParam)
		{
			auto* pNested = static_cast<Result*>(pNestedParam);
			if (!pNested->bReturned)
			{
				++pNested->iBad;
			}
			pNested->bDone = true;
		}, pState);
		pState->bReturned = true;
	}, pResult.get());
	while (!pResult->bDone.load())
	{
		co_await coro::yield(1);
	}
	static ClientConnectProbe stAwait(IpcServerPort()), stAwaitRefused("0");
	std::vector<coro::TaskResult<int>> vConnects;
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		vConnects.push_back(AwaitClientConnect(&stAwait));
	}
	for (const auto& stConnect : vConnects)
	{
		iBad += co_await stConnect;
	}
	if (stAwait.Connections() != 1 || !(co_await stAwait.StartClient()))
	{
		++iBad;
	}
	if (co_await stAwaitRefused.StartClient())
	{
		++iBad;
	}
	static ClientConnectProbe stReentrant(IpcServerPort(), true);
	const bool bFirst = co_await stReentrant.StartClient();
	const bool bRetry = co_await stReentrant.Retry();
	const int iReentrantBad = (!bFirst || bRetry || stReentrant.Connections() != 1) ? 1 : 0;
	iBad += iReentrantBad;
	printf("hold reentrant connect first=%d retry=%d connections=%d bad=%d\n", bFirst, bRetry, stReentrant.Connections(), iReentrantBad);
	stAwait.CloseAllSessions();
	stGood.CloseAllSessions();
	iBad += pResult->iBad;
	printf("hold coroutine connect bad=%d\n", iBad);
	co_return iBad;
}

struct TimerExceptionProbe
{
	uv_timer_t stWatchdog{};
	std::atomic<int> iExecuted{0};
	std::atomic<int> iDestroyed{0};
	std::atomic<bool> bClosed{false};
	std::atomic<bool> bPassed{false};
	std::atomic<int64_t> vTicks[5]{};
	uint64_t iDeadlineMilliseconds = 0;
};

struct TimerClosureProbe
{
	std::shared_ptr<TimerExceptionProbe> pState;
	explicit TimerClosureProbe(std::shared_ptr<TimerExceptionProbe> pStateParam)
	    : pState(std::move(pStateParam))
	{
	}
	~TimerClosureProbe()
	{
		++pState->iDestroyed;
	}
};

static coro::TaskResult<int> CheckTimerExceptions()
{
	auto pState = std::make_shared<TimerExceptionProbe>();
	// 帧持有状态直到 uv_close 回调完成；冻结时间轮也不会阻止独立看门狗。
	OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgumentParam)
	{
		auto* pProbe = static_cast<TimerExceptionProbe*>(pArgumentParam);
		auto* pLoop = OI::Net::Poll::GetInstance().Loop();
		if (uv_timer_init(pLoop, &pProbe->stWatchdog) != 0)
		{
			pProbe->bClosed = true;
			return;
		}
		pProbe->stWatchdog.data = pProbe;
		pProbe->iDeadlineMilliseconds = uv_now(pLoop) + 10000;
		const int iReturnCode = uv_timer_start(&pProbe->stWatchdog, [](uv_timer_t* pTimerParam)
		{
			auto* pProbeLocal = static_cast<TimerExceptionProbe*>(pTimerParam->data);
			const bool bComplete = pProbeLocal->iExecuted == 5 && pProbeLocal->iDestroyed == 5;
			if (!bComplete && uv_now(pTimerParam->loop) < pProbeLocal->iDeadlineMilliseconds)
			{
				return;
			}
			pProbeLocal->bPassed = bComplete;
			uv_timer_stop(pTimerParam);
			uv_close(reinterpret_cast<uv_handle_t*>(pTimerParam), [](uv_handle_t* pHandleParam)
			{
				static_cast<TimerExceptionProbe*>(pHandleParam->data)->bClosed = true;
			});
		}, 10, 10);
		if (iReturnCode != 0)
		{
			uv_close(reinterpret_cast<uv_handle_t*>(&pProbe->stWatchdog), [](uv_handle_t* pHandleParam)
			{
				static_cast<TimerExceptionProbe*>(pHandleParam->data)->bClosed = true;
			});
		}
	}, pState.get());
	// 在时间轮回调内布置下一批，记录实际 tick，确保覆盖同批余下回调。
	OI::Timer::BaseTimerManager::GetInstance().AddTimer([pState]()
	{
		auto& stTimers = OI::Timer::BaseTimerManager::GetInstance();
		for (int iIndex = 0; iIndex < 5; ++iIndex)
		{
			auto pClosure = std::make_shared<TimerClosureProbe>(pState);
			stTimers.AddTimer([pClosure, iIndex]()
			{
				auto pProbe = pClosure->pState;
				pProbe->vTicks[iIndex] = OI::Timer::BaseTimerManager::GetInstance().GetCurTick();
				++pProbe->iExecuted;
				if (iIndex == 0)
				{
					throw std::runtime_error("hold timer std exception");
				}
				if (iIndex == 2)
				{
					throw 73;
				}
			}, iIndex == 4 ? stTimers.GetTickOffsetMS() * 3 : 0);
		}
	}, 0);
	while (!pState->bClosed.load())
	{
		co_await coro::yield(0);
	}
	int iBad = pState->bPassed ? 0 : 1;
	if (pState->vTicks[0] != pState->vTicks[1].load() || pState->vTicks[2] != pState->vTicks[3].load() || pState->vTicks[4] <= pState->vTicks[3].load())
	{
		++iBad;
	}
	printf("hold timer exceptions executed=%d destroyed=%d bad=%d\n", pState->iExecuted.load(), pState->iDestroyed.load(), iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<int> ResultStressChild(int iValueParam)
{
	if (iValueParam & 1)
	{
		co_await coro::yield(0);
	}
	if (iValueParam % 8 == 2 || iValueParam % 8 == 3)
	{
		throw std::runtime_error("hold nested result exception");
	}
	if (iValueParam % 8 == 4 || iValueParam % 8 == 5)
	{
		throw iValueParam;
	}
	co_return iValueParam;
}

static coro::TaskResult<int> ResultStressParent(int iValueParam)
{
	co_return (co_await ResultStressChild(iValueParam)) + 1;
}

struct NestedCounts
{
	int iCompleted = 0;
	int iCaught = 0;
	int iBad = 0;
};

static coro::TaskResult<NestedCounts> NestedResultsWorker(int iWorkerParam)
{
	NestedCounts stCounts;
	for (int iIndex = 0; iIndex < 512; ++iIndex)
	{
		const int iValue = iWorkerParam * 512 + iIndex;
		try
		{
			const int iResult = co_await ResultStressParent(iValue);
			if (iResult != iValue + 1 || (iValue % 8 >= 2 && iValue % 8 <= 5))
			{
				++stCounts.iBad;
			}
		}
		catch (const std::runtime_error& stError)
		{
			++stCounts.iCaught;
			if ((iValue % 8 != 2 && iValue % 8 != 3) || std::strcmp(stError.what(), "hold nested result exception") != 0)
			{
				++stCounts.iBad;
			}
		}
		catch (int iError)
		{
			++stCounts.iCaught;
			if ((iValue % 8 != 4 && iValue % 8 != 5) || iError != iValue)
			{
				++stCounts.iBad;
			}
		}
		catch (...)
		{
			++stCounts.iBad;
		}
		++stCounts.iCompleted;
	}
	co_return stCounts;
}

static coro::TaskResult<int> CheckNestedResults()
{
	std::vector<coro::TaskResult<NestedCounts>> vWorkers;
	for (int iWorker = 0; iWorker < 64; ++iWorker)
	{
		vWorkers.push_back(NestedResultsWorker(iWorker));
	}
	NestedCounts stTotal;
	for (const auto& stWorker : vWorkers)
	{
		const NestedCounts stCounts = co_await stWorker;
		stTotal.iCompleted += stCounts.iCompleted;
		stTotal.iCaught += stCounts.iCaught;
		stTotal.iBad += stCounts.iBad;
	}
	if (stTotal.iCompleted != 32768 || stTotal.iCaught != 16384)
	{
		++stTotal.iBad;
	}
	printf("hold nested results completed=%d caught=%d bad=%d\n", stTotal.iCompleted, stTotal.iCaught, stTotal.iBad);
	fflush(stdout);
	co_return stTotal.iBad;
}

static coro::TaskResult<std::string> SharedResultChild(std::shared_ptr<std::atomic<int>> pRunsParam, bool bThrowParam)
{
	++*pRunsParam;
	co_await coro::yield(OI::Timer::Milliseconds(20));
	if (bThrowParam)
	{
		throw std::runtime_error("shared task failure");
	}
	co_return std::string(256, 's');
}
static coro::Task SharedVoidChild(std::shared_ptr<std::atomic<int>> pRunsParam)
{
	++*pRunsParam;
	co_await coro::yield(0);
}
static thread_local bool g_bInHotStartCallback = false;
static coro::Task HotStartChild(std::shared_ptr<std::atomic<int>> pStepParam, std::shared_ptr<std::atomic<bool>> pInCallbackParam = {})
{
	if (pInCallbackParam)
	{
		pInCallbackParam->store(g_bInHotStartCallback);
	}
	pStepParam->store(1);
	co_await coro::yield(0);
	pStepParam->store(2);
}
static coro::TaskResult<int> CheckHotStart()
{
	int iBad = 0;
	auto pStep = std::make_shared<std::atomic<int>>(0);
	auto stTask = HotStartChild(pStep);
	// 返回前已经跑完第一段。挂起后多线程可能已被别的工人跑完，所以不要求恰好停在 1。
	const int iStepAtCreate = pStep->load();
	if (iStepAtCreate == 0)
	{
		++iBad;
	}
	co_await stTask;
	if (pStep->load() != 2)
	{
		++iBad;
	}
	// 网络回调不在协程里，新建的任务只入队，不在回调线程上跑。
	struct Probe
	{
		std::atomic<bool> bDone{false};
		std::optional<coro::Task> stTask;
		std::shared_ptr<std::atomic<int>> pStep = std::make_shared<std::atomic<int>>(0);
		std::shared_ptr<std::atomic<bool>> pInCallback = std::make_shared<std::atomic<bool>>(true);
	};
	auto pProbe = std::make_shared<Probe>();
	OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgumentParam)
	{
		auto* pProbeLocal = static_cast<Probe*>(pArgumentParam);
		g_bInHotStartCallback = true;
		pProbeLocal->stTask.emplace(HotStartChild(pProbeLocal->pStep, pProbeLocal->pInCallback));
		g_bInHotStartCallback = false;
		pProbeLocal->bDone.store(true);
	}, pProbe.get());
	while (!pProbe->bDone.load())
	{
		co_await coro::yield(1);
	}
	co_await *pProbe->stTask;
	if (pProbe->pStep->load() != 2 || pProbe->pInCallback->load())
	{
		++iBad;
	}
	printf("hold hot start inline=%d queued=%d bad=%d\n", iStepAtCreate != 0 ? 1 : 0, pProbe->pInCallback->load() ? 0 : 1, iBad);
	fflush(stdout);
	co_return iBad;
}
// 仅用于回归统计；任务的共享所有权由 Task 自己管理。
struct SharedWaitersState
{
	std::atomic<int> iRuns{0};
	std::atomic<int> iAwakened{0};
	std::atomic<int> iBad{0};
	std::vector<std::atomic<int>> vHits;
	explicit SharedWaitersState(int iCountParam)
	    : vHits(iCountParam)
	{
	}
};
static coro::TaskResult<int> SharedWaitersChild(std::shared_ptr<SharedWaitersState> pStateParam)
{
	++pStateParam->iRuns;
	co_await coro::yield(OI::Timer::Milliseconds(20));
	co_return 42;
}
static coro::Task SharedWaiter(coro::TaskResult<int> stTaskParam, std::shared_ptr<SharedWaitersState> pStateParam, int iIndexParam)
{
	const int iResult = co_await stTaskParam;
	if (iResult != 42 || pStateParam->vHits[iIndexParam].fetch_add(1) != 0)
	{
		++pStateParam->iBad;
	}
	++pStateParam->iAwakened;
}
static coro::TaskResult<int> CheckStartedTaskWaiters()
{
	int iBad = 0;
	for (const auto& iCount : {2, 8, 64, 256})
	{
		for (int iMode = 0; iMode < 3; ++iMode)
		{
			auto pState = std::make_shared<SharedWaitersState>(iCount);
			std::vector<coro::Task> vWaiters;
			{
				auto stTask = coro::StartTask([pState](coro::Token)
				{
					return SharedWaitersChild(pState);
				});
				if (stTask.GetToken() == 0)
				{
					++pState->iBad;
				}
				if (iMode == 2 && (co_await stTask) != 42)
				{
					++pState->iBad;
				}
				for (int iIndex = 0; iIndex < iCount; ++iIndex)
				{
					// 混合场景：后一半等待者在任务自然完成后才启动。
					if (iMode == 1 && iIndex == iCount / 2 && (co_await stTask) != 42)
					{
						++pState->iBad;
					}
					vWaiters.push_back(SharedWaiter(stTask, pState, iIndex));
				}
			} // 创建者的 Task 析构，各等待者捕获的副本继续保活。
			for (const auto& stWaiter : vWaiters)
			{
				co_await stWaiter;
			}
			if (pState->iRuns.load() != 1 || pState->iAwakened.load() != iCount)
			{
				++pState->iBad;
			}
			for (const auto& iHits : pState->vHits)
			{
				if (iHits.load() != 1)
				{
					++pState->iBad;
				}
			}
			printf("hold started task mode=%d waiters=%d runs=%d awakened=%d bad=%d\n",
			    iMode, iCount, pState->iRuns.load(), pState->iAwakened.load(), pState->iBad.load());
			iBad += pState->iBad.load();
		}
	}
	co_return iBad;
}
static coro::TaskResult<int> SharedResultWaiter(coro::TaskResult<std::string> stTaskParam, bool bThrowParam)
{
	int iBad = 0;
	// 第二遍是完成后再次等待，不能重新执行，也不能拿到被移空的字符串。
	for (int iRound = 0; iRound < 2; ++iRound)
	{
		try
		{
			auto sResult = co_await stTaskParam;
			if (bThrowParam || sResult != std::string(256, 's'))
			{
				++iBad;
			}
		}
		catch (const std::runtime_error& stError)
		{
			if (!bThrowParam || std::string(stError.what()) != "shared task failure")
			{
				++iBad;
			}
		}
	}
	co_return iBad;
}
static coro::Task SharedVoidStarter(coro::Task stTaskParam)
{
	stTaskParam.Start();
	stTaskParam.Start();
	co_await stTaskParam;
	co_await stTaskParam;
}
static coro::TaskResult<int> CheckSharedTasks()
{
	int iBad = 0;
	for (int iMode = 0; iMode < 2; ++iMode)
	{
		auto pRuns = std::make_shared<std::atomic<int>>(0);
		auto stTask = SharedResultChild(pRuns, iMode != 0);
		std::vector<coro::TaskResult<int>> vWaiters;
		for (int iIndex = 0; iIndex < 64; ++iIndex)
		{
			vWaiters.push_back(SharedResultWaiter(stTask, iMode != 0));
		}
		for (const auto& stWaiter : vWaiters)
		{
			iBad += co_await stWaiter;
		}
		if (pRuns->load() != 1)
		{
			++iBad;
		}
	}
	auto pRuns = std::make_shared<std::atomic<int>>(0);
	auto stTask = SharedVoidChild(pRuns);
	std::vector<coro::Task> vStarters;
	for (int iIndex = 0; iIndex < 64; ++iIndex)
	{
		vStarters.push_back(SharedVoidStarter(stTask));
	}
	for (const auto& stStarter : vStarters)
	{
		co_await stStarter;
	}
	if (pRuns->load() != 1)
	{
		++iBad;
	}
	// 等待器独立保活：创建等待器后移动/销毁原 Task 仍安全。
	auto pLifetimeRuns = std::make_shared<std::atomic<int>>(0);
	auto pLifetimeTask = std::make_unique<coro::TaskResult<std::string>>(SharedResultChild(pLifetimeRuns, false));
	auto stWaiter = pLifetimeTask->operator co_await();
	pLifetimeTask.reset();
	if ((co_await stWaiter) != std::string(256, 's') || pLifetimeRuns->load() != 1)
	{
		++iBad;
	}
	auto stSource = SharedVoidChild(pLifetimeRuns);
	auto stMoved = std::move(stSource);
	try
	{
		co_await stSource;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	try
	{
		stSource.Start();
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	co_await stMoved;
	auto stCopy = stMoved;
	stSource = stCopy;
	auto* pSelf = &stCopy;
	stCopy = *pSelf;
	stCopy = std::move(*pSelf);
	co_await stSource;
	// 协程里新建即运行：被替换掉的任务没人等，也照样跑完一次。
	auto pUnusedRuns = std::make_shared<std::atomic<int>>(0);
	auto stReplaced = SharedVoidChild(pUnusedRuns);
	stReplaced = std::move(stCopy);
	co_await stReplaced;
	if (pLifetimeRuns->load() != 2 || pUnusedRuns->load() != 1)
	{
		++iBad;
	}
	printf("hold shared tasks waiters=64 bad=%d\n", iBad);
	co_return iBad;
}

class DeferredRpcProbe : public OI::Rpc::ProtoClientManager
{
  public:
	DeferredRpcProbe()
	{
		DefaultInit("127.0.0.1", IpcServerPort());
	}
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
	}
};

// 常驻测试管理器，使用真实会话及固定发送额度，不绕过底层发送判定。
class DirectBudgetProbe : public DeferredRpcProbe
{
  public:
	DirectBudgetProbe()
	{
		iMaxSendQueueBytes = 256;
		SetPendingCallLimits(16, 4096);
	}

  protected:
	bool AutoReconnect() const override
	{
		return false;
	}
};

static coro::TaskResult<int> CheckDirectBackpressure()
{
	static DirectBudgetProbe stClient;
	int iBad = 0;
	OI::RpcData::AllEchoReq stLarge;
	stLarge.stData.sMessage.assign(512, 'B');
	OI::RpcData::AllEchoRes stOutput;
	stOutput.stData.sMessage = "unchanged";
	// 当前任务内调用立即启动，连接尚未开始，稳定进入建连前排队路径。
	auto stQueued = OI::Rpc::RpcCall(stClient, stLarge, stOutput, OI::Timer::Seconds(3));
	const bool bConnected = co_await stClient.StartClient();
	const int32_t iQueuedRet = co_await stQueued;
	if (!bConnected || iQueuedRet != OI_ERR_BUSY || stOutput.stData.sMessage != "unchanged")
	{
		++iBad;
	}
	const sid_t iSid = stClient.ConnectedSid();
	const int32_t iDirectRet = co_await OI::Rpc::RpcCall(stClient, stLarge, stOutput, OI::Timer::Seconds(3));
	const int32_t iPinnedRet = co_await OI::Rpc::RpcCall(stClient, iSid, stLarge, stOutput, OI::Timer::Seconds(3));
	if (iDirectRet != OI_ERR_BUSY || iPinnedRet != OI_ERR_BUSY || stOutput.stData.sMessage != "unchanged" || stClient.ConnectedSid() != iSid)
	{
		++iBad;
	}
	OI::RpcData::HoldReq stSmall;
	OI::RpcData::HoldRes stSmallOutput;
	stSmall.iMark = 975;
	const int32_t iRecoveryRet = co_await OI::Rpc::RpcCall(stClient, stSmall, stSmallOutput, OI::Timer::Seconds(3));
	if (iRecoveryRet != OI_OK || stSmallOutput.iMark != 975)
	{
		++iBad;
	}
	stClient.CloseAllSessions();
	const int32_t iClosedRet = co_await OI::Rpc::RpcCall(stClient, iSid, stSmall, stSmallOutput, OI::Timer::Seconds(3));
	if (iClosedRet != OI_ERR_CONNECT)
	{
		++iBad;
	}
	printf("hold direct backpressure queued=%d direct=%d pinned=%d closed=%d bad=%d\n", iQueuedRet, iDirectRet, iPinnedRet, iClosedRet, iBad);
	co_return iBad;
}

// 只统计提交，仍走真实连接、序列化、服务端 Hold 和回包。
class OutputRpcProbe : public DeferredRpcProbe
{
	std::atomic<int> m_iSubmissions{0};

  public:
	void SubmitCall(uint64_t iIdParam, const OI::Base::Bytes& stDataParam) override
	{
		m_iSubmissions.fetch_add(1, std::memory_order_relaxed);
		ProtoClientManager::SubmitCall(iIdParam, stDataParam);
	}
	int Submissions() const
	{
		return m_iSubmissions.load(std::memory_order_relaxed);
	}
};

static coro::TaskResult<int> AwaitOutputRpc(coro::TaskResult<int32_t> stTaskParam)
{
	try
	{
		co_return (co_await stTaskParam) == 0 ? 0 : 1;
	}
	catch (const std::exception&)
	{
		co_return 1;
	}
}

static coro::TaskResult<int> CheckOutputRpcTask(sid_t iSessionIdParam)
{
	int iBad = 0;
	static OutputRpcProbe stClient;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -1;
	const int iBefore = stClient.Submissions();
	// 普通函数体返回 Task 后局部请求消失；管理器和输出活到所有等待者完成。
	auto stCall = [pOutput = &stOutput]()
	{
		OI::RpcData::HoldReq stLocal;
		stLocal.iDelayMilliseconds = 100;
		stLocal.iMark = 731;
		auto stTask = OI::Rpc::RpcCall(stClient, stLocal, *pOutput, OI::Timer::Seconds(3));
		stLocal.iMark = -731;
		return stTask;
	}();
	static_assert(std::is_same_v<decltype(stCall), coro::TaskResult<int32_t>>);
	std::vector<coro::TaskResult<int>> vWaiters;
	for (int iIndex = 0; iIndex < 16; ++iIndex)
	{
		vWaiters.push_back(coro::StartTask([stCall](coro::Token)
		{
			return AwaitOutputRpc(stCall);
		}));
	}
	if (!(co_await stClient.StartClient()))
	{
		++iBad;
	}
	for (const auto& stWaiter : vWaiters)
	{
		iBad += co_await stWaiter;
	}
	if (stOutput.iMark != 731)
	{
		++iBad;
	}
	// 任务已完成。修改输出，再从多个任务重复 await，不能重新发送或再次写输出。
	stOutput.iMark = 732;
	vWaiters.clear();
	for (int iIndex = 0; iIndex < 16; ++iIndex)
	{
		vWaiters.push_back(coro::StartTask([stCall](coro::Token)
		{
			return AwaitOutputRpc(stCall);
		}));
	}
	for (const auto& stWaiter : vWaiters)
	{
		iBad += co_await stWaiter;
	}
	const int iSubmitted = stClient.Submissions() - iBefore;
	if (iSubmitted != 1 || stOutput.iMark != 732)
	{
		++iBad;
	}
	printf("hold output rpc shared waiters=16 submissions=%d bad=%d\n", iSubmitted, iBad);

	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 733;
	auto& stManager = IpcClient::GetInstance();
	auto stDirect = stManager.Call(stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stDirect), coro::TaskResult<int32_t>>);
	if (co_await stDirect != 0 || stOutput.iMark != 733)
	{
		++iBad;
	}
	stRequest.iMark = 734;
	auto stPinned = OI::Rpc::RpcCall(stManager, iSessionIdParam, stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stPinned), coro::TaskResult<int32_t>>);
	if (co_await stPinned != 0 || stOutput.iMark != 734)
	{
		++iBad;
	}
	static DeferredRpcProbe stUnconnected;
	const auto stBefore = OI::Rpc::GetRpcWaitStats();
	if (co_await OI::Rpc::RpcCall(stUnconnected, sid_t(12345), stRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_CONNECT ||
	    stOutput.iMark != 734 || OI::Rpc::GetRpcWaitStats().iCalls != stBefore.iCalls)
	{
		++iBad;
	}
	stRequest.iDelayMilliseconds = -1;
	auto stError = stManager.Call(stRequest, stOutput, OI::Timer::Seconds(3));
	if (co_await stError != 1001 || co_await stError != 1001 || stOutput.iMark != 734)
	{
		++iBad;
	}

	stRequest.iDelayMilliseconds = 1000;
	stRequest.iMark = 735;
	auto stTimeout = stManager.Call(stRequest, stOutput, OI::Timer::Milliseconds(100));
	if (co_await stTimeout != OI_ERR_TIMEOUT || stOutput.iMark != 734)
	{
		++iBad;
	}
	// 复用同一输出给下一次成功请求，迟到回包不得覆盖新结果。
	stRequest.iDelayMilliseconds = 0;
	stRequest.iMark = 736;
	if (co_await stManager.Call(stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 736)
	{
		++iBad;
	}
	co_await coro::yield(OI::Timer::Milliseconds(1200));
	if (stOutput.iMark != 736 || co_await stTimeout != OI_ERR_TIMEOUT || stOutput.iMark != 736)
	{
		++iBad;
	}
	stClient.CloseAllSessions();
	printf("hold output rpc direct pinned error late bad=%d\n", iBad);
	fflush(stdout);
	co_return iBad;
}

template <typename T>
static coro::TaskResult<bool> AwaitSingleUse(std::shared_ptr<T> pCallParam)
{
	try
	{
		(void)(co_await *pCallParam);
	}
	catch (const std::logic_error&)
	{
		co_return false;
	}
	co_return true;
}

// 两次并发等待只能接受一次；完成后再等也必须拒绝。
template <typename T>
static coro::TaskResult<int> CheckSingleUse(std::shared_ptr<T> pCallParam)
{
	auto pReadyCount = std::make_shared<std::atomic<int>>(0);
	auto fnContend = [pCallParam, pReadyCount](coro::Token) -> coro::TaskResult<bool>
	{
		pReadyCount->fetch_add(1, std::memory_order_acq_rel);
		// 两个独立任务都就绪后再争抢；单线程等待时让出调度。
		while (pReadyCount->load(std::memory_order_acquire) < 2)
		{
			co_await coro::yield(0);
		}
		co_return co_await AwaitSingleUse(pCallParam);
	};
	auto stFirst = coro::StartTask(fnContend);
	auto stSecond = coro::StartTask(fnContend);
	int iAccepted = (co_await stFirst ? 1 : 0) + (co_await stSecond ? 1 : 0);
	if (co_await AwaitSingleUse(pCallParam))
	{
		++iAccepted;
	}
	co_return iAccepted == 1 ? 0 : 1;
}

class SharedConnectProbe : public DeferredRpcProbe
{
  public:
	void Pending()
	{
		bConnectPending = true;
	}
	void Finish()
	{
		AbortSession(iConnectGeneration);
	}
	void OnConnectFailed() override
	{
	}
};

static coro::TaskResult<bool> AwaitConnect(std::shared_ptr<OI::Net::SessionManager::ConnectAwaiter> pConnectParam)
{
	co_return co_await *pConnectParam;
}

static coro::TaskResult<int> CheckAwaitOwnership()
{
	int iBad = 0;
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 50;
	stRequest.iMark = 123;
	// 先解码再移动响应：避免构造结果时提前取走仍未填充的对象。
	auto stDecodeProbe = OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest);
	OI::RpcData::HoldRes stDecodedSource;
	stDecodedSource.iMark = 456;
	stDecodeProbe.stReply << int32_t(0);
	stDecodedSource.serialize(stDecodeProbe.stReply);
	stDecodeProbe.pResponse = &stDecodeProbe.stResponse;
	stDecodeProbe.bReplied = true;
	auto stDecodedResult = stDecodeProbe.await_resume();
	if (stDecodedResult.GetRet() != 0 || stDecodedResult.GetRes().iMark != 456)
	{
		++iBad;
	}
	auto stRpc = OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, OI::Timer::Seconds(3));
	auto pRpc = std::make_shared<decltype(stRpc)>(std::move(stRpc));
	try
	{
		co_await stRpc;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	iBad += co_await CheckSingleUse(pRpc);
	OI::RpcData::HoldRes stOutput;
	auto stOutputWait = OI::Rpc::MakeRpcCallAwaiter(IpcClient::GetInstance(), stRequest, stOutput, OI::Timer::Seconds(3));
	iBad += co_await CheckSingleUse(std::make_shared<decltype(stOutputWait)>(std::move(stOutputWait)));
	if (stOutput.iMark != 123)
	{
		++iBad;
	}
	try
	{
		auto stInvalidMove = std::move(*pRpc);
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	OI::Http::Client stInvalidHttp("", 0);
	auto stHttp = stInvalidHttp.Get("/", {});
	auto pHttp = std::make_shared<OI::Http::Call>(std::move(stHttp));
	try
	{
		co_await stHttp;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	iBad += co_await CheckSingleUse(pHttp);
	int32_t iHttpPort = 0;
	TryGetConfig("Regression.http_port", iHttpPort);
	if (iHttpPort > 0)
	{
		OI::Http::Client stHttpClient("127.0.0.1", iHttpPort);
		iBad += co_await CheckSingleUse(std::make_shared<OI::Http::Call>(stHttpClient.Get("/good", {})));
	}
	static SharedConnectProbe stClient;
	OI::Net::Poll::GetInstance().CallOnLoop([](void*)
	{
		stClient.Pending();
	}, nullptr);
	auto pConnect = std::make_shared<OI::Net::SessionManager::ConnectAwaiter>(stClient.StartClient());
	// 每个等待者创建时就跑到挂起，循环结束后 32 个都已登记。
	std::vector<coro::TaskResult<bool>> vWaiters;
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		vWaiters.push_back(AwaitConnect(pConnect));
	}
	OI::Net::Poll::GetInstance().CallOnLoop([](void*)
	{
		stClient.Finish();
	}, nullptr);
	for (const auto& stWaiter : vWaiters)
	{
		if (co_await stWaiter)
		{
			++iBad;
		}
	}
	if (co_await *pConnect)
	{
		++iBad;
	}
	auto stMovedConnect = std::move(*pConnect);
	try
	{
		co_await *pConnect;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	if (co_await stMovedConnect)
	{
		++iBad;
	}
	// 固定不存在的 sid 不得进入首次连接队列，也不能等待超时。
	static DeferredRpcProbe stPinnedClient;
	const auto stBefore = OI::Rpc::GetRpcWaitStats();
	auto stPinned = co_await OI::Rpc::RpcCall(stPinnedClient, sid_t(12345), stRequest, OI::Timer::Seconds(30));
	const auto stAfter = OI::Rpc::GetRpcWaitStats();
	if (stPinned.GetRet() != OI_ERR_CONNECT || stAfter.iCalls != stBefore.iCalls)
	{
		++iBad;
	}
	printf("hold await ownership shared=32 bad=%d\n", iBad);
	fflush(stdout);
	co_return iBad;
}

// 成功且标记对上返回 0，其他结果原样返回状态码。
static coro::TaskResult<int32_t> AdmissionCall(DeferredRpcProbe* pClientParam)
{
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 1000;
	stRequest.iMark = 71;
	auto stResult = co_await OI::Rpc::RpcCall(*pClientParam, stRequest, OI::Timer::Seconds(5));
	if (stResult.GetRet() == 0 && stResult.GetRes().iMark != 71)
	{
		co_return OI_ERR_FAIL;
	}
	co_return stResult.GetRet();
}

static coro::TaskResult<int> CheckRpcAdmission()
{
	static DeferredRpcProbe vClients[3];
	int iBad = 0;
	for (auto& stClient : vClients)
	{
		if (!(co_await stClient.StartClient()))
		{
			co_return 1;
		}
	}
	for (int iRound = 0; iRound < 3; ++iRound)
	{
		std::vector<coro::TaskResult<int32_t>> vCalls;
		for (int iClient = 0; iClient < 3; ++iClient)
		{
			for (int iIndex = 0; iIndex < 2; ++iIndex)
			{
				vCalls.push_back(AdmissionCall(&vClients[iClient]));
			}
		}
		int iOk = 0, iBusy = 0, iFailed = 0;
		int vAccepted[3]{};
		for (size_t iCall = 0; iCall < vCalls.size(); ++iCall)
		{
			const int32_t iReturnCode = co_await vCalls[iCall];
			if (iReturnCode == 0)
			{
				++iOk;
				++vAccepted[iCall / 2];
			}
			else if (iReturnCode == OI_ERR_BUSY)
			{
				++iBusy;
			}
			else
			{
				++iFailed;
			}
		}
		if (iOk != 2 || iBusy != 4 || iFailed != 0)
		{
			++iBad;
		}
		for (const int iAccepted : vAccepted)
		{
			if (iAccepted > 1)
			{
				++iBad;
			}
		}
		printf("hold rpc admission round=%d ok=%d busy=%d bad=%d\n", iRound, iOk, iBusy, iBad);
		co_await coro::yield(50);
	}
	// 超大请求拒绝不占额度，错误回执也必须归还；随后正常请求仍可执行。
	bool bBytes = false;
	TryGetConfig("Regression.rpc_admission_bytes", bBytes);
	if (bBytes)
	{
		struct LargeRequest : OI::RpcData::HoldReq
		{
			OI::Base::BytesStream& serialize(OI::Base::BytesStream& stOutputParam) const override
			{
				HoldReq::serialize(stOutputParam);
				stOutputParam.WritePb(99, std::string(128, 'x'));
				return stOutputParam;
			}
		} stLarge;
		OI::RpcData::HoldReq& stBase = stLarge;
		OI::RpcData::HoldRes stOutput;
		if ((co_await OI::Rpc::MakeRpcCallAwaiter(vClients[0], stBase, stOutput, OI::Timer::Seconds(3))) != OI_ERR_BUSY)
		{
			++iBad;
		}
	}
	for (int iKind = 0; iKind < 4; ++iKind)
	{
		OI::RpcData::HoldReq stRequest;
		stRequest.iDelayMilliseconds = iKind == 0 ? -1 : 0;
		stRequest.iMark = iKind == 1 ? -2147483600 : (iKind == 2 ? -2147483599 : 71);
		auto stResult = co_await OI::Rpc::RpcCall(vClients[0], stRequest, OI::Timer::Seconds(3));
		const int iExpected = iKind == 0 ? 1001 : (iKind < 3 ? OI_ERR_FAIL : 0);
		if (stResult.GetRet() != iExpected)
		{
			++iBad;
		}
		co_await coro::yield(50);
	}
	for (auto& stClient : vClients)
	{
		stClient.CloseAllSessions();
	}
	printf("hold rpc admission done bad=%d\n", iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<int> CheckDeferredRpcSession()
{
	static DeferredRpcProbe stClient;
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 0;
	stRequest.iMark = 31415;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -1;
	auto stValueWait = OI::Rpc::RpcCall(stClient, stRequest, OI::Timer::Seconds(3));
	auto stOutputWait = OI::Rpc::RpcCall(stClient, stRequest, stOutput, OI::Timer::Seconds(3));
	int iBad = 0;
	int iValueCode = OI_ERR_CONNECT;
	int iOutputCode = OI_ERR_CONNECT;
	int iPinnedCode = 0;
	if (!(co_await stClient.StartClient()))
	{
		++iBad;
		// 输出接口创建即运行，失败路径也要等任务结束再释放输出。
		iOutputCode = co_await stOutputWait;
	}
	else
	{
		auto stValue = co_await stValueWait;
		iValueCode = stValue.GetRet();
		iOutputCode = co_await stOutputWait;
		if (iValueCode != 0 || stValue.GetRes().iMark != 31415 || iOutputCode != 0 || stOutput.iMark != 31415)
		{
			++iBad;
		}
		auto stPinned = co_await OI::Rpc::RpcCall(stClient, INVALID_SID, stRequest, OI::Timer::Seconds(3));
		iPinnedCode = stPinned.GetRet();
		if (iPinnedCode != OI_ERR_CONNECT)
		{
			++iBad;
		}
	}
	stClient.CloseAllSessions();
	printf("hold deferred rpc value=%d output=%d pinned=%d bad=%d\n", iValueCode, iOutputCode, iPinnedCode, iBad);
	fflush(stdout);
	co_return iBad;
}

// 重复 StartClient 不能顶掉已有连接；RPC 和 Redis 各自收到自己的回包。
static coro::TaskResult<int> IsolationCall(int iIndexParam)
{
	int iBad = 0;
	IpcClient::GetInstance().StartClient();
	OI::Redis::RedisClient::GetInstance().StartClient();
	OI::Log::LogClient::GetInstance().StartClient();
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 10000 + iIndexParam;
	auto stResult = co_await IpcClient::GetInstance().CallAny<OI::RpcData::HoldRes>(stRequest, OI::Timer::Seconds(3));
	if (stResult.GetRet() != 0 || stResult.GetRes().iMark != stRequest.iMark)
	{
		++iBad;
	}
	std::vector<std::string> vArguments{"ECHO", std::to_string(iIndexParam)};
	auto stReply = co_await OI::Redis::CallRedis(std::move(vArguments));
	if (!stReply || !stReply.pReply->str || std::string(stReply.pReply->str, stReply.pReply->len) != std::to_string(iIndexParam))
	{
		++iBad;
	}
	co_return iBad;
}

static coro::TaskResult<int> CheckConnectionIsolation()
{
	int iBad = 0;
	auto& stClient = IpcClient::GetInstance();
	auto& stRedis = OI::Redis::RedisClient::GetInstance();
	const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(5)).iMilliseconds;
	while (!stRedis.IsConnect() && ::OI::Timer::SteadyMilliseconds() < iDeadline)
	{
		co_await coro::yield(1);
	}
	const sid_t iRpcSid = stClient.ConnectedSid();
	const sid_t iRedisSid = stRedis.ConnectedSid();
	if (iRpcSid == INVALID_SID || iRedisSid == INVALID_SID)
	{
		co_return 1;
	}
	std::vector<coro::TaskResult<int>> vCalls;
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		vCalls.push_back(IsolationCall(iIndex));
	}
	for (const auto& stCall : vCalls)
	{
		iBad += co_await stCall;
	}
	if (stClient.ConnectedSid() != iRpcSid || stRedis.ConnectedSid() != iRedisSid)
	{
		++iBad;
	}
	struct ProbeResult
	{
		std::atomic<bool> bDone{false};
		int iBad = 0;
	};
	auto pProbe = std::make_shared<ProbeResult>();
	// 协程帧持有 pProbe，回调完成前不退出；回调结束后只读已发布的结果。
	OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgumentParam)
	{
		auto* pResult = static_cast<ProbeResult*>(pArgumentParam);
		ConnectGenerationProbe stProbe;
		pResult->iBad = stProbe.Check();
		auto& stRedisLocal = OI::Redis::RedisClient::GetInstance();
		stRedisLocal.Feed(INVALID_SID, "not RESP", 8);
		stRedisLocal.Feed(stRedisLocal.ConnectedSid() + 1, "+FOREIGN\r\n", 10);
		pResult->bDone.store(true);
	}, pProbe.get());
	while (!pProbe->bDone.load())
	{
		co_await coro::yield(1);
	}
	if (!Pong(co_await OI::Redis::CallRedis({"PING"})))
	{
		++iBad;
	}
	iBad += pProbe->iBad;
	printf("hold connection isolation bad=%d\n", iBad);
	co_return iBad;
}

class ZoneService final : public ::OI::Rpc::ProtoClientManager
{
  public:
	explicit ZoneService(std::string sNameParam)
	{
		sServiceName = std::move(sNameParam);
	}
	const std::string GetIDString()
	{
		return "ZoneService";
	}
	void OnClientConnect(::OI::Net::Session*)
	{
	}
	void OnClientDisconnect(const ::OI::Net::Session*)
	{
	}
};

enum class NamedOutcome
{
	Success,
	Failed,
	Rejected,
};

static coro::TaskResult<bool> MoveRouteProbe(ZoneService* pManagerParam, std::string sIpParam, std::string sPortParam)
{
	struct ServiceMove
	{
		ZoneService* pManager;
		std::string sIp;
		std::string sPort;
		std::exception_ptr stError;
		std::atomic<bool> bDone{false};
	};
	auto pMove = std::make_shared<ServiceMove>();
	pMove->pManager = pManagerParam;
	pMove->sIp = std::move(sIpParam);
	pMove->sPort = std::move(sPortParam);
	// 帧保活状态直到 poll 发布完成；模拟注册更新，地址只在 poll 线程修改。
	OI::Net::Poll::GetInstance().CallOnLoop([](void* pArgumentParam)
	{
		auto* pMoveLocal = static_cast<ServiceMove*>(pArgumentParam);
		try
		{
			pMoveLocal->pManager->ApplyService(pMoveLocal->sIp, pMoveLocal->sPort, true);
		}
		catch (...)
		{
			pMoveLocal->stError = std::current_exception();
		}
		pMoveLocal->bDone.store(true, std::memory_order_release);
	}, pMove.get());
	while (!pMove->bDone.load(std::memory_order_acquire))
	{
		co_await coro::yield(1);
	}
	if (pMove->stError)
	{
		std::rethrow_exception(pMove->stError);
	}
	co_return co_await pManagerParam->StartClient();
}

static coro::TaskResult<int> CheckRouteTargetStability()
{
	OI::Rpc::RouteAwaiter stPoolWait;
	stPoolWait.iKind = OI::Rpc::RouteAwaiter::Kind::Zones;
	stPoolWait.sName = "gameserver";
	stPoolWait.vZones = {1001, 1002};
	auto stPool = co_await stPoolWait;
	if (stPool.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stPool.vClients.size() != 2 ||
	    !stPool.vClients[0] || !stPool.vClients[1])
	{
		co_return 1;
	}
	static ZoneService stMoving("gameserver");
	auto* pFirst = stPool.vClients[0];
	auto* pSecond = stPool.vClients[1];
	if (!(co_await MoveRouteProbe(&stMoving, OI::Rpc::GetRouteConnectionInfo(*pFirst).sIp, OI::Rpc::GetRouteConnectionInfo(*pFirst).sPort)))
	{
		co_return 1;
	}
	OI::Rpc::RouteAwaiter stSingleWait;
	stSingleWait.pManager = &stMoving;
	stSingleWait.sName = "gameserver";
	stSingleWait.iPick = OI::Rpc::ServicePick::Zone;
	stSingleWait.sZone = "1001";
	auto stSingle = co_await stSingleWait;
	OI::Rpc::RouteAwaiter stManyWait;
	stManyWait.pManager = &stMoving;
	stManyWait.iKind = OI::Rpc::RouteAwaiter::Kind::Zones;
	stManyWait.sName = "gameserver";
	stManyWait.vZones = {1002, 1001};
	auto stMany = co_await stManyWait;
	// 刻意在选路和提交之间完成换区，稳定覆盖竞态窗口，不依赖压力碰运气。
	if (!(co_await MoveRouteProbe(&stMoving, OI::Rpc::GetRouteConnectionInfo(*pSecond).sIp, OI::Rpc::GetRouteConnectionInfo(*pSecond).sPort)))
	{
		co_return 1;
	}
	int iBad = 0;
	if (stSingle.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stSingle.vClients.size() != 1 ||
	    stSingle.vClients[0] != pFirst || stMany.iStatus != OI::Rpc::ServiceRouteStatus::Ok ||
	    stMany.vClients.size() != 2 || stMany.vClients[0] != pSecond || stMany.vClients[1] != pFirst)
	{
		++iBad;
	}
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 1001;
	if (stSingle.vClients.size() == 1 && stSingle.vClients[0])
	{
		auto stReply = co_await OI::Rpc::RpcCall(*stSingle.vClients[0], stRequest, OI::Timer::Seconds(3));
		if (stReply.GetRet() != 0 || stReply.GetRes().iMark != stRequest.iMark)
		{
			++iBad;
		}
	}
	auto vReplies = co_await OI::Rpc::SendRouted<OI::RpcData::HoldRes>(std::move(stMany), stRequest, OI::Timer::Seconds(3));
	if (vReplies.size() != 2)
	{
		++iBad;
	}
	for (const auto& stReply : vReplies)
	{
		if (stReply.GetRet() != 0 || stReply.GetRes().iMark != stRequest.iMark)
		{
			++iBad;
		}
	}
	stMoving.CloseAllSessions();
	printf("hold route target stability bad=%d\n", iBad);
	co_return iBad;
}

template <typename T>
static coro::TaskResult<NamedOutcome> AwaitNamedHold(T stCallParam)
{
	try
	{
		auto stReply = co_await stCallParam;
		co_return stReply.GetRet() == 0 && stReply.GetRes().iMark == 77 ? NamedOutcome::Success : NamedOutcome::Failed;
	}
	catch (const std::logic_error&)
	{
	}
	co_return NamedOutcome::Rejected;
}

class RoutePolicyProbe : public OI::Rpc::ProtoClientManager
{
  public:
	explicit RoutePolicyProbe(size_t iScaleParam)
	{
		sServiceName = "gameserver";
		iMaxSendQueueBytes = 256 * iScaleParam;
		iMaxReceiveBufferBytes = 512 * iScaleParam + 8;
		iMaxFrameBytes = 512 * iScaleParam;
		iIncompleteFrameTimeoutMilliseconds = 300 * iScaleParam;
		SetPendingCallLimits(2 * iScaleParam, 1024 * iScaleParam);
	}
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
	}
};

static coro::TaskResult<int> CheckRoutePoolLifetime()
{
	int iBad = 0;
	RoutePolicyProbe stSmall(1), stLarge(16);
	OI::Rpc::RouteAwaiter stSmallWait, stLargeWait;
	stSmallWait.pManager = &stSmall;
	stLargeWait.pManager = &stLarge;
	for (auto* pWait : {&stSmallWait, &stLargeWait})
	{
		pWait->sName = "gameserver";
		pWait->iPick = OI::Rpc::ServicePick::Zone;
		pWait->sZone = "1001";
	}
	auto stSmallRoute = co_await stSmallWait;
	auto stLargeRoute = co_await stLargeWait;
	if (stSmallRoute.vClients.size() != 1 || stLargeRoute.vClients.size() != 1 ||
	    stSmallRoute.vOwners.size() != 1 || stLargeRoute.vOwners.size() != 1)
	{
		co_return 1;
	}
	auto* pSmall = stSmallRoute.vClients[0];
	auto* pLarge = stLargeRoute.vClients[0];
	std::weak_ptr<void> pSmallOwner = OI::Rpc::GetRouteConnectionOwner(*pSmall);
	std::weak_ptr<void> pLargeOwner = OI::Rpc::GetRouteConnectionOwner(*pLarge);
	for (const auto& stPair : {std::make_pair(pSmall, &stSmall), std::make_pair(pLarge, &stLarge)})
	{
		if (OI::Rpc::GetRouteConnectionInfo(*stPair.first).iSendBytes != stPair.second->MaxSendQueueBytes() ||
		    OI::Rpc::GetRouteConnectionInfo(*stPair.first).iReceiveBytes != stPair.second->MaxReceiveBufferBytes() ||
		    OI::Rpc::GetRouteConnectionInfo(*stPair.first).iFrameBytes != stPair.second->MaxFrameBytes() ||
		    OI::Rpc::GetRouteConnectionInfo(*stPair.first).iIncompleteMilliseconds != stPair.second->IncompleteFrameTimeoutMilliseconds() ||
		    OI::Rpc::GetRouteConnectionInfo(*stPair.first).stPendingLimits != stPair.second->PendingCallLimits())
		{
			++iBad;
		}
	}
	if (pSmall == pLarge || &pSmall->Manager() != &pLarge->Manager())
	{
		++iBad;
	}
	OI::RpcData::HoldReq stWarmRequest;
	OI::RpcData::HoldRes stWarmResponse;
	stWarmRequest.iMark = 912;
	if (co_await OI::Rpc::RpcCall(*pSmall, stWarmRequest, stWarmResponse, OI::Timer::Seconds(3)) != OI_OK)
	{
		++iBad;
	}
	const auto iWarmSid = OI::Rpc::GetRouteConnectionInfo(*pSmall).iSessionId;
	OI::RpcData::AllEchoReq stLargeRequest;
	stLargeRequest.stData.sMessage.assign(512, 'P');
	OI::RpcData::AllEchoRes stOutput;
	if (co_await OI::Rpc::RpcCall(*pSmall, stLargeRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_BUSY ||
	    co_await OI::Rpc::RpcCall(*pLarge, stLargeRequest, stOutput, OI::Timer::Seconds(3)) != 0 ||
	    stOutput.stData.sMessage != stLargeRequest.stData.sMessage)
	{
		++iBad;
	}
	if (OI::Rpc::GetRouteConnectionInfo(*pSmall).iSessionId != iWarmSid)
	{
		++iBad;
	}
	// 连接前缓存能装下该包，但连接后的发送预算不足，也必须准确返回 BUSY。
	pSmall->Manager().CloseSession(iWarmSid);
	if (co_await OI::Rpc::RpcCall(*pSmall, stLargeRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_BUSY ||
	    co_await OI::Rpc::RpcCall(*pSmall, stWarmRequest, stWarmResponse, OI::Timer::Seconds(3)) != OI_OK || stWarmResponse.iMark != 912)
	{
		++iBad;
	}
	printf("hold route backpressure status bad=%d\n", iBad);
	const auto stBefore = OI::Rpc::GetRoutePoolStats();
	OI::RpcData::HoldReq stRequest;
	stRequest.iDelayMilliseconds = 800;
	stRequest.iMark = 821;
	OI::RpcData::HoldRes stHeldOutput;
	auto stHeld = OI::Rpc::RpcCall(*pLarge, stRequest, stHeldOutput, OI::Timer::Seconds(3));
	co_await coro::yield(OI::Timer::Milliseconds(600));
	if (OI::Rpc::GetRoutePoolStats().iLeasedClients < 2 || co_await stHeld != 0 || stHeldOutput.iMark != 821)
	{
		++iBad;
	}
	// 弱引用指向业务lease，不把它变成额外保活引用；释放路由后等待真实闲置扫描。
	std::weak_ptr<OI::Rpc::RpcSender> pSmallLease = stSmallRoute.vOwners[0];
	std::weak_ptr<OI::Rpc::RpcSender> pLargeLease = stLargeRoute.vOwners[0];
	stSmallRoute = {};
	stLargeRoute = {};
	co_await coro::yield(OI::Timer::Milliseconds(600));
	const int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
	while ((!pSmallOwner.expired() || !pLargeOwner.expired()) && OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
	{
		co_await coro::yield(20);
	}
	const auto stAfter = OI::Rpc::GetRoutePoolStats();
	if (!pSmallLease.expired() || !pLargeLease.expired() || !pSmallOwner.expired() || !pLargeOwner.expired() ||
	    stAfter.iEvicted < stBefore.iEvicted + 2 ||
	    stAfter.iClients > stAfter.iMaxClients)
	{
		++iBad;
	}
	stRequest.iDelayMilliseconds = 0;
	if (co_await stLarge.Call(1001, stRequest, stHeldOutput, OI::Timer::Seconds(3)) != 0 || stHeldOutput.iMark != 821)
	{
		++iBad;
	}
	printf("hold route policy isolation idle reconnect bad=%d\n", iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<int> CheckRouteInboundAndReconnect()
{
	int iBad = 0;
	static ZoneService stGames("gameserver");
	OI::Rpc::RouteAwaiter stWait;
	stWait.pManager = &stGames;
	stWait.sName = "gameserver";
	stWait.iPick = OI::Rpc::ServicePick::Zone;
	stWait.sZone = "1001";
	auto stRoute = co_await stWait;
	if (stRoute.vClients.size() != 1)
	{
		co_return 1;
	}
	auto* pClient = stRoute.vClients[0];
	OI::RpcData::HoldReq stRequest;
	OI::RpcData::HoldRes stOutput;
	stRequest.iMark = 845;
	if (co_await OI::Rpc::RpcCall(*pClient, stRequest, stOutput, OI::Timer::Seconds(3)) != 0)
	{
		++iBad;
	}
	for (int iRound = 0; iRound < 4; ++iRound)
	{
		pClient->Manager().CloseSession(OI::Rpc::GetRouteConnectionInfo(*pClient).iSessionId);
		// 不让出、不等 poll 清理：直接向同一连接发送，必须排队重连。
		if (co_await OI::Rpc::RpcCall(*pClient, stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 845)
		{
			++iBad;
		}
	}
	printf("hold pooled reconnect bad=%d\n", iBad);
	std::weak_ptr<void> pWeakOwner = OI::Rpc::GetRouteConnectionOwner(*pClient);
	stRequest.iMark = -2147483598;
	if (co_await OI::Rpc::RpcCall(*pClient, stRequest, stOutput, OI::Timer::Seconds(3)) != 0)
	{
		++iBad;
	}
	const int64_t iStartDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
	while (iReverseStarted.load() == 0 && OI::Timer::SteadyMilliseconds() < iStartDeadlineMilliseconds)
	{
		co_await coro::yield(10);
	}
	stRoute = {}; // 唯一出站租约释放，反向处理仍应保活且阻止空闲关闭。
	co_await coro::yield(600);
	if (iReverseStarted.load() != 1 || iReverseFinished.load() != 0 || pWeakOwner.expired())
	{
		++iBad;
	}
	const int64_t iEndDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 5000;
	while (!pWeakOwner.expired() && OI::Timer::SteadyMilliseconds() < iEndDeadlineMilliseconds)
	{
		co_await coro::yield(20);
	}
	if (iReverseFinished.load() != 1 || !pWeakOwner.expired())
	{
		++iBad;
	}
	printf("hold inbound route lifetime bad=%d\n", iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<int> CheckRoutePoolCapacity()
{
	int iBad = 0;
	int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
	while (OI::Rpc::GetRoutePoolStats().iClients != 0 && OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
	{
		co_await coro::yield(20);
	}
	const auto stEmpty = OI::Rpc::GetRoutePoolStats();
	if (stEmpty.iClients != 0 || stEmpty.iMaxClients != 8)
	{
		co_return 1;
	}
	OI::Base::ServiceInstance stInstance;
	stInstance.sName = "ipcserver";
	stInstance.iId = 1;
	stInstance.sIp = "127.0.0.1";
	stInstance.sPort = IpcServerPort();
	std::vector<OI::Base::ServiceInstance> vInstances{stInstance};
	std::vector<std::unique_ptr<RoutePolicyProbe>> vPolicies;
	std::vector<OI::Rpc::ServiceRoute> vRoutes;
	for (size_t iIndex = 0; iIndex < stEmpty.iMaxClients; ++iIndex)
	{
		vPolicies.push_back(std::make_unique<RoutePolicyProbe>(20 + iIndex));
		vRoutes.push_back(OI::Rpc::FinishRoute("ipcserver", OI::Rpc::ServicePick::Any, "", 0, vInstances, vPolicies.back().get()));
		if (vRoutes.back().iStatus != OI::Rpc::ServiceRouteStatus::Ok || vRoutes.back().vOwners.size() != 1)
		{
			++iBad;
		}
	}
	RoutePolicyProbe stOverflow(100);
	auto stRejected = OI::Rpc::FinishRoute("ipcserver", OI::Rpc::ServicePick::Any, "", 0, vInstances, &stOverflow);
	if (stRejected.iStatus != OI::Rpc::ServiceRouteStatus::Busy || !stRejected.vClients.empty() || !stRejected.vOwners.empty())
	{
		++iBad;
	}
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 822;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -822;
	const int32_t iBusy = co_await stOverflow.CallAny(stRequest, stOutput, OI::Timer::Seconds(3));
	if (iBusy != OI_ERR_BUSY || stOutput.iMark != -822 || OI::Rpc::GetRoutePoolStats().iClients != 8)
	{
		++iBad;
	}
	// 池满不影响已持有的合法路由，且活跃租约跨扫描仍可真实收发。
	co_await coro::yield(600);
	if (vRoutes[0].vClients.size() == 1)
	{
		if (co_await OI::Rpc::RpcCall(*vRoutes[0].vClients[0], stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 822)
		{
			++iBad;
		}
	}
	vRoutes.clear();
	iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
	while (OI::Rpc::GetRoutePoolStats().iClients != 0 && OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
	{
		co_await coro::yield(20);
	}
	if (OI::Rpc::GetRoutePoolStats().iClients != 0 || co_await stOverflow.CallAny(stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 822)
	{
		++iBad;
	}
	printf("hold route pool capacity busy=%d bad=%d\n", iBusy == OI_ERR_BUSY ? 1 : 0, iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<bool> RegistryFaultHandshake(std::string sPathParam, std::string sCommandParam)
{
	{
		std::ofstream stFile(sPathParam + ".request", std::ios::trunc);
		stFile << sCommandParam;
		if (!stFile)
		{
			co_return false;
		}
	}
	const int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 5000;
	while (OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
	{
		std::ifstream stFile(sPathParam + ".ack");
		std::string sAck;
		stFile >> sAck;
		if (sAck == sCommandParam)
		{
			co_return true;
		}
		co_await coro::yield(10);
	}
	co_return false;
}

static coro::TaskResult<int> CheckRegistryBoundsAndBudget()
{
	std::string sFaultPath;
	TryGetConfig("Regression.registry_fault_path", sFaultPath);
	if (sFaultPath.empty())
	{
		co_return 1;
	}
	int iBad = 0;
	auto& stRegistry = OI::Base::RegistryClient::GetInstance();
	const auto stBefore = stRegistry.GetPullStats();
	if (!(co_await RegistryFaultHandshake(sFaultPath, "pause")))
	{
		co_return 1;
	}
	int iBusy = 0, iTimeout = 0;
	static ZoneService stManyProbe("gameserver");
	OI::RpcData::HoldReq stManyRequest;
	const std::vector<int32_t> vManyZones{1001, 1002, 1001};
	const auto vTimedOut = co_await stManyProbe.CallMany<OI::RpcData::HoldRes>(vManyZones, stManyRequest, OI::Timer::Milliseconds(100));
	int iManyBad = vTimedOut.size() == vManyZones.size() ? 0 : 1;
	for (const auto& stResult : vTimedOut)
	{
		iManyBad += stResult.GetRet() != OI_ERR_TIMEOUT;
	}
	struct PullProbe
	{
		std::atomic<int> iCompleted{0};
		std::atomic<int> iDuplicates{0};
		std::atomic<int> iBusy{0};
		std::atomic<int> iTimeout{0};
		std::atomic<int> iUnexpected{0};
	};
	std::vector<std::shared_ptr<PullProbe>> vProbes;
	for (int iRound = 0; iRound < 3; ++iRound)
	{
		auto pProbe = std::make_shared<PullProbe>();
		vProbes.push_back(pProbe);
		for (int iIndex = 0; iIndex < 64; ++iIndex)
		{
			auto pSeen = std::make_shared<std::atomic<bool>>(false);
			stRegistry.PullAsync("missing_timeout_" + std::to_string(iIndex), [pProbe, pSeen](int32_t iCodeParam, const std::vector<OI::Base::ServiceInstance>&)
			{
				if (pSeen->exchange(true))
				{
					++pProbe->iDuplicates;
					return;
				}
				if (iCodeParam == OI_ERR_BUSY)
				{
					++pProbe->iBusy;
				}
				else if (iCodeParam == OI_ERR_TIMEOUT)
				{
					++pProbe->iTimeout;
				}
				else if (iCodeParam != OI_ERR_CONNECT)
				{
					++pProbe->iUnexpected;
				}
				++pProbe->iCompleted;
			}, 200);
			const auto stStats = stRegistry.GetPullStats();
			if (stStats.iLiveCount > static_cast<size_t>(stStats.iMaxPendingPulls) ||
			    stStats.iLiveCount != stStats.iSentCount + stStats.iDeferredCount)
			{
				++iBad;
			}
		}
		if (iRound == 0)
		{
			const auto vBusy = co_await stManyProbe.CallMany<OI::RpcData::HoldRes>(vManyZones, stManyRequest, OI::Timer::Seconds(3));
			iManyBad += vBusy.size() != vManyZones.size();
			for (const auto& stResult : vBusy)
			{
				iManyBad += stResult.GetRet() != OI_ERR_BUSY;
			}
		}
		const int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + 3000;
		while (pProbe->iCompleted.load() < 64 && OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds)
		{
			co_await coro::yield(10);
		}
		co_await coro::yield(100);
		if (pProbe->iCompleted.load() != 64 || pProbe->iDuplicates.load() != 0 || pProbe->iUnexpected.load() != 0 ||
		    stRegistry.GetPendingPullCount() != 0)
		{
			++iBad;
		}
		iBusy += pProbe->iBusy.load();
		iTimeout += pProbe->iTimeout.load();
	}
	if (iBusy == 0 || iTimeout == 0 || stRegistry.GetPullStats().iResetCount <= stBefore.iResetCount)
	{
		++iBad;
	}
	if (!(co_await RegistryFaultHandshake(sFaultPath, "resume")))
	{
		co_return iBad + 1;
	}
	// 让后台订阅拉取完成，再用真实已知分区验证恢复后回包配对。
	co_await coro::yield(1200);
	static ZoneService stGames("gameserver");
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 831;
	OI::RpcData::HoldRes stOutput;
	if (co_await stGames.Call(1001, stRequest, stOutput, OI::Timer::Seconds(3)) != 0 || stOutput.iMark != 831)
	{
		++iBad;
	}
	for (const auto& pProbe : vProbes)
	{
		if (pProbe->iCompleted.load() != 64 || pProbe->iDuplicates.load() != 0 || pProbe->iUnexpected.load() != 0)
		{
			++iBad;
		}
	}
	const auto vInvalid = co_await stManyProbe.CallMany<OI::RpcData::HoldRes>({1001, -1, 1002}, stManyRequest, OI::Timer::Seconds(3));
	iManyBad += vInvalid.size() != 3;
	for (const auto& stResult : vInvalid)
	{
		iManyBad += stResult.GetRet() != OI_ERR_FAIL;
	}
	iBad += iManyBad;
	printf("hold batch failure slots bad=%d\n", iManyBad);
	printf("hold registry bounded recovery busy=%d timeout=%d bad=%d\n", iBusy, iTimeout, iBad);
	fflush(stdout);
	if (!(co_await RegistryFaultHandshake(sFaultPath, "budget")))
	{
		co_return iBad + 1;
	}
	// runner 在确认暂停后600ms恢复；查询+900ms服务处理超过1000ms总预算。
	stRequest.iDelayMilliseconds = 900;
	stOutput.iMark = -831;
	OI::RpcData::HoldRes stStringOutput;
	stStringOutput.iMark = -831;
	const int64_t iBudgetStartMilliseconds = OI::Timer::SteadyMilliseconds();
	auto stNamed = stGames.Call(1001, stRequest, stOutput, OI::Timer::Milliseconds(1000));
	auto stString = OI::Rpc::RpcCall(std::string("gameserver.1002"), stRequest, stStringOutput, OI::Timer::Milliseconds(1000));
	auto stValue = stGames.Call<OI::RpcData::HoldRes>(1001, stRequest, OI::Timer::Milliseconds(1000));
	auto stBatch = stGames.CallAll<OI::RpcData::HoldRes>(stRequest, OI::Timer::Milliseconds(1000));
	const auto iNamedCode = co_await stNamed;
	const auto iStringCode = co_await stString;
	const auto stValueResult = co_await stValue;
	const auto vBatchResults = co_await stBatch;
	const long long iBudgetElapsedMilliseconds = OI::Timer::SteadyMilliseconds() - iBudgetStartMilliseconds;
	if (iNamedCode != OI_ERR_TIMEOUT || iStringCode != OI_ERR_TIMEOUT || stValueResult.GetRet() != OI_ERR_TIMEOUT ||
	    stOutput.iMark != -831 || stStringOutput.iMark != -831 || vBatchResults.size() != 2 || iBudgetElapsedMilliseconds >= 1450)
	{
		++iBad;
	}
	for (const auto& stResult : vBatchResults)
	{
		if (stResult.GetRet() != OI_ERR_TIMEOUT)
		{
			++iBad;
		}
	}
	co_await coro::yield(1000);
	if (stOutput.iMark != -831 || stStringOutput.iMark != -831)
	{
		++iBad;
	}
	printf("hold named rpc end to end budget elapsed_ms=%lld bad=%d\n", iBudgetElapsedMilliseconds, iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<int> CheckCompletedRpcRequests(sid_t iSessionIdParam)
{
	int iBad = 0;
	static ZoneService stGames("gameserver");
	auto& stManager = IpcClient::GetInstance();
	auto pDestroyed = std::make_shared<std::atomic<int>>(0);
	std::vector<OI::RpcData::HoldRes> vOutputs(20);
	std::vector<std::weak_ptr<HoldRequestResource>> vResources;
	std::vector<coro::TaskResult<int32_t>> vTasks;
	// 六个公开输出入口各测成功、业务失败、序列化异常，再测超时和选路失败。
	for (int iIndex = 0; iIndex < 20; ++iIndex)
	{
		ResourceHoldRequest stRequest;
		stRequest.pResource = std::make_shared<HoldRequestResource>(pDestroyed);
		vResources.emplace_back(stRequest.pResource);
		stRequest.iMark = 900 + iIndex;
		stRequest.iDelayMilliseconds = iIndex < 18 && iIndex % 3 == 1 ? -1 : 50;
		stRequest.bThrow = iIndex < 18 && iIndex % 3 == 2;
		vOutputs[iIndex].iMark = -900;
		switch (iIndex / 3)
		{
		case 0:
			vTasks.push_back(OI::Rpc::RpcCall(stManager, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 1:
			vTasks.push_back(OI::Rpc::RpcCall(stManager, iSessionIdParam, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 2:
			vTasks.push_back(stManager.Call(std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 3:
			vTasks.push_back(stGames.CallAny(std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 4:
			vTasks.push_back(stGames.Call(1001, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		case 5:
			vTasks.push_back(OI::Rpc::RpcCall(std::string("gameserver.1002"), std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			break;
		default:
			if (iIndex == 18)
			{
				stRequest.iDelayMilliseconds = 700;
				vTasks.push_back(stManager.Call(std::move(stRequest), vOutputs[iIndex], OI::Timer::Milliseconds(100)));
			}
			else
			{
				vTasks.push_back(stGames.Call(9999, std::move(stRequest), vOutputs[iIndex], OI::Timer::Seconds(3)));
			}
			break;
		}
	}
	// 保留全部 Task，重复读取保存的结果/异常；不能靠清空任务容器释放请求。
	for (int iPass = 0; iPass < 2; ++iPass)
	{
		for (int iIndex = 0; iIndex < 20; ++iIndex)
		{
			const bool bExpectedException = iIndex < 18 && iIndex % 3 == 2;
			const int32_t iExpected = iIndex == 18 ? OI_ERR_TIMEOUT : (iIndex == 19 ? OI_ERR_CONNECT : (iIndex % 3 == 1 ? 1001 : 0));
			bool bCaught = false;
			try
			{
				if (co_await vTasks[iIndex] != iExpected)
				{
					++iBad;
				}
			}
			catch (const std::runtime_error& stError)
			{
				bCaught = std::string(stError.what()) == "request resource serialization failure";
				if (!bCaught)
				{
					++iBad;
				}
			}
			catch (const std::exception&)
			{
				++iBad;
			}
			const int32_t iExpectedMark = !bExpectedException && iExpected == 0 ? 900 + iIndex : -900;
			if (bCaught != bExpectedException || vOutputs[iIndex].iMark != iExpectedMark || !vResources[iIndex].expired())
			{
				++iBad;
			}
		}
	}
	// 超时任务的迟到回包也不能重新使用已释放请求或覆盖保留的输出。
	co_await coro::yield(OI::Timer::Milliseconds(800));
	if (vOutputs[18].iMark != -900 || pDestroyed->load(std::memory_order_relaxed) != 20)
	{
		++iBad;
	}
	printf("hold completed rpc requests retained=%zu destroyed=%d bad=%d\n", vTasks.size(), pDestroyed->load(), iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<int> CheckNamedOutputRpcTask()
{
	int iBad = 0;
	static ZoneService stGames("gameserver");
	auto& stManager = IpcClient::GetInstance();
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = 811;
	OI::RpcData::HoldRes stOutput;
	stOutput.iMark = -1;
	auto stAny = stManager.CallAny(stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stAny), coro::TaskResult<int32_t>>);
	if (co_await stAny != 0 || stOutput.iMark != 811)
	{
		++iBad;
	}
	stRequest.iMark = 812;
	auto stZone = stGames.Call(1001, stRequest, stOutput, OI::Timer::Seconds(3));
	static_assert(std::is_same_v<decltype(stZone), coro::TaskResult<int32_t>>);
	if (co_await stZone != 0 || stOutput.iMark != 812)
	{
		++iBad;
	}
	// 服务名和请求仅在普通 lambda 的调用栈上存在，返回后任务必须自己保活。
	auto stNamed = [pOutput = &stOutput]()
	{
		OI::RpcData::HoldReq stLocal;
		stLocal.iDelayMilliseconds = 100;
		stLocal.iMark = 813;
		std::string sName = "gameserver.1002";
		auto stTask = OI::Rpc::RpcCall(sName, stLocal, *pOutput, OI::Timer::Seconds(3));
		sName = "missing.9999";
		stLocal.iMark = -813;
		return stTask;
	}();
	static_assert(std::is_same_v<decltype(stNamed), coro::TaskResult<int32_t>>);
	std::vector<coro::TaskResult<int>> vWaiters;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		vWaiters.push_back(coro::StartTask([stNamed](coro::Token)
		{
			return AwaitOutputRpc(stNamed);
		}));
	}
	for (const auto& stWaiter : vWaiters)
	{
		iBad += co_await stWaiter;
	}
	if (stOutput.iMark != 813)
	{
		++iBad;
	}
	stOutput.iMark = 814;
	if (co_await stAny != 0 || co_await stZone != 0 || co_await stNamed != 0 || stOutput.iMark != 814)
	{
		++iBad;
	}
	stRequest.iDelayMilliseconds = -1;
	if (co_await stGames.CallAny(stRequest, stOutput, OI::Timer::Seconds(3)) != 1001 || stOutput.iMark != 814)
	{
		++iBad;
	}
	if (co_await stGames.Call(9999, stRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_CONNECT || stOutput.iMark != 814)
	{
		++iBad;
	}
	if (co_await OI::Rpc::RpcCall(std::string("gameserver.*"), stRequest, stOutput, OI::Timer::Seconds(3)) != OI_ERR_FAIL || stOutput.iMark != 814)
	{
		++iBad;
	}
	printf("hold output rpc named any zone shared=8 bad=%d\n", iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<int> CheckNamedServiceRpc()
{
	int iBad = 0;
	static ZoneService stGames("gameserver");
	auto& stIpc = IpcClient::GetInstance();
	::OI::RpcData::AllEchoReq stRequest;
	int iGlobal = 0;
	int iAny = 0;
	int iZone = 0;
	int iBatch = 0;
	for (int iTry = 0; iTry < 10; ++iTry)
	{
		auto stGlobal = co_await stIpc.CallAny<::OI::RpcData::AllEchoRes>(stRequest, ::OI::Timer::Seconds(3));
		auto stAny = co_await stGames.CallAny<::OI::RpcData::AllEchoRes>(stRequest, ::OI::Timer::Seconds(3));
		auto stZone = co_await stGames.Call<::OI::RpcData::AllEchoRes>(1001, stRequest, ::OI::Timer::Seconds(3));
		auto vBatch = co_await stGames.CallAll<::OI::RpcData::AllEchoRes>(stRequest, ::OI::Timer::Seconds(3));
		iGlobal = stGlobal.GetRet() == 0 && stGlobal.GetRes().iCode == 1 ? 1 : 0;
		iAny = stAny.GetRet() == 0 && stAny.GetRes().iCode == 1 ? 1 : 0;
		iZone = stZone.GetRet() == 0 && stZone.GetRes().iCode == 1 ? 1 : 0;
		iBatch = 0;
		for (const auto& stItem : vBatch)
		{
			if (stItem.GetRet() == 0 && stItem.GetRes().iCode == 1)
			{
				++iBatch;
			}
		}
		if (iGlobal == 1 && iAny == 1 && iZone == 1 && iBatch == 2)
		{
			break;
		}
		co_await coro::yield(::OI::Timer::Milliseconds(200));
	}
	if (iGlobal != 1 || iAny != 1 || iZone != 1 || iBatch != 2)
	{
		++iBad;
	}
	auto stWrongZone = co_await stIpc.Call<::OI::RpcData::AllEchoRes>(1001, stRequest, ::OI::Timer::Seconds(3));
	int iWrong = stWrongZone.GetRet() == OI_ERR_FAIL ? 1 : 0;
	if (iWrong != 1)
	{
		++iBad;
	}
	auto stMissed = co_await stGames.Call<::OI::RpcData::AllEchoRes>(9999, stRequest, ::OI::Timer::Seconds(3));
	int iMiss = stMissed.GetRet() == OI_ERR_CONNECT ? 1 : 0;
	if (iMiss != 1)
	{
		++iBad;
	}
	std::vector<int32_t> vBothZones{1001, 1002};
	auto vMany = co_await stGames.CallMany<::OI::RpcData::AllEchoRes>(vBothZones, stRequest, ::OI::Timer::Seconds(3));
	int iMany = 0;
	if (vMany.size() == vBothZones.size())
	{
		for (const auto& stItem : vMany)
		{
			if (stItem.GetRet() == 0 && stItem.GetRes().iCode == 1)
			{
				++iMany;
			}
		}
	}
	std::vector<int32_t> vPartialZones{1001, 9999};
	auto vPartial = co_await stGames.CallMany<::OI::RpcData::AllEchoRes>(vPartialZones, stRequest, ::OI::Timer::Seconds(3));
	int iSlot = vPartial.size() == 2 && vPartial[0].GetRet() == 0 && vPartial[0].GetRes().iCode == 1 && vPartial[1].GetRet() == OI_ERR_CONNECT ? 1 : 0;
	auto vEmptyMany = co_await stGames.CallMany<::OI::RpcData::AllEchoRes>(std::vector<int32_t>{}, stRequest, ::OI::Timer::Seconds(3));
	auto vGlobalMany = co_await stIpc.CallMany<::OI::RpcData::AllEchoRes>(vBothZones, stRequest, ::OI::Timer::Seconds(3));
	if (iMany != 2 || iSlot != 1 || !vEmptyMany.empty() || vGlobalMany.size() != 2 ||
	    vGlobalMany[0].GetRet() != OI_ERR_FAIL || vGlobalMany[1].GetRet() != OI_ERR_FAIL)
	{
		++iBad;
	}
	::OI::RpcData::HoldReq stHold;
	stHold.iDelayMilliseconds = 400;
	stHold.iMark = 77;
	int64_t iBegin = ::OI::Timer::SteadyMilliseconds();
	auto vHeld = co_await stGames.CallAll<::OI::RpcData::HoldRes>(stHold, ::OI::Timer::Seconds(3));
	long long iElapsed = ::OI::Timer::SteadyMilliseconds() - iBegin;
	int iHeldOk = 0;
	for (const auto& stItem : vHeld)
	{
		if (stItem.GetRet() == 0 && stItem.GetRes().iMark == 77)
		{
			++iHeldOk;
		}
	}
	if (iHeldOk != 2 || iElapsed >= 700)
	{
		++iBad;
	}
	printf("hold service rpc global=%d any=%d zone=%d batch=%d many=%d slot=%d elapsed_ms=%lld wrong=%d miss=%d bad=%d\n", iGlobal, iAny, iZone, iBatch, iMany, iSlot, iElapsed, iWrong, iMiss, iBad);
	// 服务名只在入口解析；局部字符串和请求销毁后，延迟启动仍能正确发送。
	static ZoneService stOfflineWatcher("ipcserver");
	OI::Base::ServiceTable::GetInstance().Watch(&stOfflineWatcher, "ipcserver");
	OI::Rpc::RouteAwaiter stFallbackWait;
	stFallbackWait.pManager = &stOfflineWatcher;
	stFallbackWait.sName = "ipcserver";
	auto stFallback = co_await stFallbackWait;
	if (stFallback.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stFallback.vClients.size() != 1 || stFallback.vClients[0] == &stOfflineWatcher.Sender())
	{
		++iBad;
	}
	auto stDeferred = []()
	{
		::OI::RpcData::HoldReq stLocal;
		stLocal.iMark = 913;
		std::string sTarget = "gameserver.*";
		auto stTask = OI::Rpc::RpcCallBatch<OI::RpcData::HoldRes>(sTarget, stLocal, OI::Timer::Seconds(3));
		sTarget = "invalid.*";
		return stTask;
	}();
	auto stDeferredMany = []()
	{
		::OI::RpcData::HoldReq stLocal;
		stLocal.iMark = 914;
		return stGames.CallMany<OI::RpcData::HoldRes>({1001, 1002}, stLocal, OI::Timer::Seconds(3));
	}();
	co_await coro::yield(0);
	auto vDeferred = co_await stDeferred;
	if (vDeferred.size() != 2)
	{
		++iBad;
	}
	for (const auto& stItem : vDeferred)
	{
		if (stItem.GetRet() != 0 || stItem.GetRes().iMark != 913)
		{
			++iBad;
		}
	}
	for (const auto& stItem : co_await stDeferredMany)
	{
		if (stItem.GetRet() != 0 || stItem.GetRes().iMark != 914)
		{
			++iBad;
		}
	}
	auto stSingle = stGames.Call<OI::RpcData::HoldRes>(1001, stHold, OI::Timer::Seconds(3));
	std::vector<coro::TaskResult<NamedOutcome>> vContenders;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		vContenders.push_back(AwaitNamedHold(stSingle));
	}
	int iSuccess = 0, iRejected = 0;
	for (const auto& stContender : vContenders)
	{
		const NamedOutcome iOutcome = co_await stContender;
		iSuccess += iOutcome == NamedOutcome::Success ? 1 : 0;
		iRejected += iOutcome == NamedOutcome::Rejected ? 1 : 0;
	}
	auto stAgain = co_await stSingle;
	if (stAgain.GetRet() != 0 || stAgain.GetRes().iMark != 77)
	{
		++iBad;
	}
	if (iSuccess != 8 || iRejected != 0)
	{
		++iBad;
	}
	printf("hold named rpc ownership success=%d rejected=%d bad=%d\n", iSuccess, iRejected, iBad);
	fflush(stdout);
	co_return iBad;
}

static coro::TaskResult<int> RegistryLookups()
{
	int iBad = 0;
	for (int iRound = 0; iRound < 8; ++iRound)
	{
		// 周期拉取与同步查询交错，队列顺序必须和线上顺序一致。
		OI::Base::RegistryClient::GetInstance().Pull();
		std::string sIp, sPort;
		if (!OI::Base::RegistryClient::GetInstance().Lookup("ipcserver", sIp, sPort) || sIp != "127.0.0.1" || sPort != IpcServerPort())
		{
			++iBad;
		}
		co_await coro::yield(0);
	}
	co_return iBad;
}

static coro::TaskResult<int> CheckRegistryPairing()
{
	std::vector<coro::TaskResult<int>> vLookups;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		vLookups.push_back(RegistryLookups());
	}
	int iBad = 0;
	for (const auto& stLookup : vLookups)
	{
		iBad += co_await stLookup;
	}
	printf("hold registry pairing bad=%d\n", iBad);
	co_return iBad;
}

// 异常必须释放闭包；随后继续执行普通任务，不能终止工作线程。
static coro::TaskResult<int> CheckTaskExceptions()
{
	int iBadLocal = 0;
	for (int iKind = 0; iKind < 2; ++iKind)
	{
		auto pOwner = std::make_shared<int>(1);
		std::weak_ptr<int> pWeak = pOwner;
		if (!OI::Thread::ThreadPool::GetInstance().AddTask([pOwner, iKind]()
		{
			if (iKind == 0)
			{
				throw std::runtime_error("ordinary task regression");
			}
			throw 7;
		}))
		{
			++iBadLocal;
		}
		pOwner.reset();
		const int64_t iDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
		while (!pWeak.expired() && ::OI::Timer::SteadyMilliseconds() < iDeadline)
		{
			co_await coro::yield(1);
		}
		if (!pWeak.expired())
		{
			++iBadLocal;
		}
	}
	auto pOrdinary = std::make_shared<std::atomic<bool>>(false);
	if (!OI::Thread::ThreadPool::GetInstance().AddTask([pOrdinary]()
	{
		pOrdinary->store(true);
	}))
	{
		++iBadLocal;
	}
	const int64_t iTaskDeadline = ::OI::Timer::SteadyMilliseconds() + ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	while (!pOrdinary->load() && ::OI::Timer::SteadyMilliseconds() < iTaskDeadline)
	{
		co_await coro::yield(1);
	}
	if (!pOrdinary->load())
	{
		++iBadLocal;
	}
	printf("hold task exceptions bad=%d\n", iBadLocal);
	co_return iBadLocal;
}

static coro::TaskResult<int> CheckServerEncoding()
{
	int iBadLocal = 0;
	OI::RpcData::HoldReq stRequest;
	stRequest.iMark = -2147483600;
	OI::RpcData::HoldRes stResponse;
	stResponse.iMark = 91;
	int32_t iResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
	if (iResult != OI_ERR_FAIL || stResponse.iMark != 91)
	{
		++iBadLocal;
	}
	stRequest.iMark = -2147483599;
	iResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
	if (iResult != OI_ERR_FAIL || stResponse.iMark != 91)
	{
		++iBadLocal;
	}
	const std::string sOriginal("error\n\r\0\\%s", 11);
	const OI::Log::DiagnosticText stEscaped(sOriginal);
	const OI::Log::DiagnosticText stLong(std::string(5000, 'X'));
	const OI::Log::DiagnosticText stExact(std::string(4096, 'X'));
	if (std::string(stEscaped.GetText()) != "error\\x0a\\x0d\\x00\\x5c%s" || stEscaped.IsTruncated() ||
	    !stLong.IsTruncated() || std::strlen(stLong.GetText()) != 4096 || stExact.IsTruncated())
	{
		++iBadLocal;
	}
	stRequest.iMark = -2147483597;
	stResponse.iMark = 91;
	iResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
	if (iResult != OI_ERR_FAIL || stResponse.iMark != 91)
	{
		++iBadLocal;
	}
	printf("hold client malformed response sid=%llu bad=%d\n", (unsigned long long)IpcClient::GetInstance().ConnectedSid(), iBadLocal);
	stRequest.iMark = 92;
	auto stRecovered = co_await IpcClient::GetInstance().CallAny<OI::RpcData::HoldRes>(stRequest, OI::Timer::Seconds(2));
	if (stRecovered.GetRet() != 0 || stRecovered.GetRes().iMark != 92)
	{
		++iBadLocal;
	}
	printf("hold server encoding bad=%d\n", iBadLocal);
	co_return iBadLocal;
}

static coro::TaskResult<int> CheckHttpFraming(int32_t iHttpPortParam)
{
	int iBadLocal = 0;
	OI::Http::Client stHttp("127.0.0.1", iHttpPortParam, 1000);
	struct HttpCase
	{
		const char* pPath;
		int iStatus;
		bool bFail;
		const char* pBody;
	};
	const std::vector<HttpCase> vCases{
	    {"good", 200, false, "ok"}, {"interim", 200, false, "ok"}, {"fragmented", 200, false, "ok"}, {"204", 204, false, ""}, {"304", 304, false, ""}, {"chunk-good", 200, false, "ok"}, {"length-junk", 200, true, ""}, {"length-overflow", 200, true, ""}, {"length-conflict", 200, true, ""}, {"te-cl", 200, true, ""}, {"chunk-separator", 200, true, ""}, {"chunk-junk", 200, true, ""}, {"truncated", 200, true, ""}, {"only-interim", 100, true, ""}, {"good", 200, false, "ok"}};
	for (const auto& stCase : vCases)
	{
		auto stResult = co_await stHttp.Get(std::string("/") + stCase.pPath, {});
		if (stResult.bFail != stCase.bFail || (!stCase.bFail && (stResult.iStatus != stCase.iStatus || stResult.sBody != stCase.pBody)))
		{
			++iBadLocal;
			printf("http regression path=%s status=%d fail=%d error=%s\n", stCase.pPath, stResult.iStatus, stResult.bFail, stResult.sError.c_str());
		}
	}
	printf("hold http framing bad=%d\n", iBadLocal);
	co_return iBadLocal;
}

static size_t AllocatedBytes()
{
#if defined(__GLIBC__)
#if __GLIBC_PREREQ(2, 33)
	const auto stMemory = mallinfo2();
#else
	const auto stMemory = mallinfo();
#endif
	return static_cast<size_t>(stMemory.uordblks) + static_cast<size_t>(stMemory.hblkhd);
#else
	return 0;
#endif
}

// 长超时期间连续成功的大响应，不应把已完成的收包容量留到超时。
static coro::TaskResult<int> CheckHttpRelease(int32_t iHttpPortParam)
{
	int iBadLocal = 0;
	OI::Http::Client stLargeHttp("127.0.0.1", iHttpPortParam, 30000);
	const size_t iBefore = AllocatedBytes();
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		auto stResult = co_await stLargeHttp.Get("/large", {});
		if (stResult.bFail || stResult.sBody != std::string(1024 * 1024, 'x'))
		{
			++iBadLocal;
		}
	}
	const size_t iAfter = AllocatedBytes();
	if (iAfter > iBefore + 8 * 1024 * 1024)
	{
		++iBadLocal;
	}
	printf("hold http release before=%zu after=%zu bad=%d\n", iBefore, iAfter, iBadLocal);
	co_return iBadLocal;
}

// 发送预算必须跨会话/通道缓冲连续计数，真正写出才恢复额度。
static int CheckSendBudget()
{
	int iBadLocal = 0;
	struct BufferedSession : OI::Net::NetSession
	{
		BufferedSession()
		    : NetSession(nullptr)
		{
		}
		void OnDataIn() override
		{
		}
		void OnDataOut() override
		{
		}
	} stBuffered;
	stBuffered.SetSendQueueLimit(8);
	OI::Base::Bytes stPayload("12345678", 8), stStaged;
	if (stBuffered.SendResult(stPayload) != OI_OK || stBuffered.SendResult(stPayload) != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	stBuffered.SwapOutBuff(stStaged);
	if (stBuffered.PendingSendBytes() != 8 || stBuffered.Send(stPayload))
	{
		++iBadLocal;
	}
	stBuffered.OnBytesSent(8);
	stStaged.Clear();
	if (!stBuffered.Send(stPayload))
	{
		++iBadLocal;
	}
	stBuffered.ActiveClose();
	if (!stBuffered.IsClosed() || stBuffered.SendResult(stPayload) != OI_ERR_CONNECT)
	{
		++iBadLocal;
	}
	printf("hold send budget bad=%d\n", iBadLocal);
	return iBadLocal;
}

// 持续就绪的协程不能饿死同优先级普通任务。
static coro::TaskResult<int> CheckSchedulerFairness()
{
	int iBadLocal = 0;
	std::atomic<bool> bOrdinaryDone{false};
	coro::TaskScope stBusyScope;
	const int64_t iFairnessBegin = ::OI::Timer::SteadyMilliseconds();
	const int64_t iFairnessLimit = ::OI::Timer::MS(::OI::Timer::Seconds(3)).iMilliseconds;
	for (int iIndexLocal = 0; iIndexLocal < 64; ++iIndexLocal)
	{
		stBusyScope.Start([&bOrdinaryDone, iFairnessBegin, iFairnessLimit](coro::Token, coro::StopToken stStopParam) -> coro::Task
		{
			while (!bOrdinaryDone.load() && !stStopParam.StopRequested() && ::OI::Timer::SteadyMilliseconds() - iFairnessBegin < iFairnessLimit)
			{
				co_await coro::yield(0);
			}
		});
	}
	co_await coro::yield(OI::Timer::Milliseconds(50));
	if (!OI::Thread::ThreadPool::GetInstance().AddTask([&bOrdinaryDone]()
	{
		bOrdinaryDone.store(true);
	}))
	{
		++iBadLocal;
		bOrdinaryDone.store(true);
	}
	while (!bOrdinaryDone.load())
	{
		co_await coro::yield(0);
	}
	if (WallMs(iFairnessBegin) > 1500)
	{
		++iBadLocal;
	}
	stBusyScope.RequestStop();
	co_await stBusyScope.Join();
	printf("hold scheduler fairness bad=%d\n", iBadLocal);
	co_return iBadLocal;
}

// 子任务异常跨挂起点传回父任务；作用域关闭后不再接受任务。
static coro::TaskResult<int> CheckTaskScope()
{
	int iBadLocal = 0;
	auto fnThrowing = [](coro::Token, coro::StopToken) -> coro::Task
	{
		co_await coro::yield(::OI::Timer::Milliseconds(10));
		throw std::runtime_error("scope regression");
	};
	coro::TaskScope stFailures;
	stFailures.Start(fnThrowing);
	bool bCaught = false;
	try
	{
		co_await stFailures.Join();
	}
	catch (const std::runtime_error&)
	{
		bCaught = true;
	}
	if (!bCaught)
	{
		++iBadLocal;
	}
	coro::TaskScope stScope;
	std::atomic<int> iFinishedLocal{0}, iStarted{0};
	for (int iIndexLocal = 0; iIndexLocal < 128; ++iIndexLocal)
	{
		stScope.Start([&iFinishedLocal, &iStarted](coro::Token, coro::StopToken stStopParam) -> coro::Task
		{
			++iStarted;
			while (!stStopParam.StopRequested())
			{
				co_await coro::yield(::OI::Timer::Milliseconds(10));
			}
			++iFinishedLocal;
		});
	}
	while (iStarted.load() != 128)
	{
		co_await coro::yield(::OI::Timer::Milliseconds(10));
	}
	stScope.RequestStop();
	co_await stScope.Join();
	if (iFinishedLocal.load() != 128)
	{
		++iBadLocal;
	}
	try
	{
		stScope.Start(fnThrowing);
		++iBadLocal;
	}
	catch (const std::logic_error&)
	{
	}
	printf("hold scope finished=%d caught=%d bad=%d\n", iFinishedLocal.load(), bCaught, iBadLocal);
	co_return iBadLocal;
}

// 真正的带捕获协程 lambda：临时闭包离开后、挂起恢复后仍访问捕获。
static coro::TaskResult<int> CheckCapturedTasks()
{
	std::vector<coro::TaskResult<int>> vTasks;
	for (int iIndexLocal = 0; iIndexLocal < 32; ++iIndexLocal)
	{
		vTasks.push_back(coro::StartTask([text = std::string(4096, 'a' + iIndexLocal % 26), iIndexLocal](coro::Token iTokenParam) -> coro::TaskResult<int>
		{
			co_await coro::yield(::OI::Timer::Milliseconds(0));
			co_await coro::yield(::OI::Timer::Milliseconds(10));
			co_return (!iTokenParam || text != std::string(4096, 'a' + iIndexLocal % 26)) ? 1 : 0;
		}));
	}
	int iBad = 0;
	for (const auto& stTask : vTasks)
	{
		iBad += co_await stTask;
	}
	printf("hold captured tasks done=%zu bad=%d\n", vTasks.size(), iBad);
	co_return iBad;
}

// 使用真实待连接队列验证条数、字节限制以及取消后的容量恢复。
struct PendingClient : OI::Rpc::ProtoClientManager
{
	int iFailureMode = 0;
	void SetSendLimit(size_t iBytesParam)
	{
		iMaxSendQueueBytes = iBytesParam;
	}
	OI::Rpc::ProtoSession* AttachForTest()
	{
		auto pSessionLocal = new OI::Rpc::ProtoSession(this);
		AddSession(pSessionLocal);
		return pSessionLocal;
	}
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
	}
	void SubmitCall(uint64_t iIdParam, const OI::Base::Bytes& stDataParam) override
	{
		if (!iFailureMode)
		{
			ProtoClientManager::SubmitCall(iIdParam, stDataParam);
			return;
		}
		if (iFailureMode == 2)
		{
			OI::Rpc::WakeRpcCall(iIdParam, OI_ERR_FAIL, nullptr);
		}
		if (iFailureMode == 3)
		{
			ProtoClientManager::TryPendCall(iIdParam, stDataParam);
		}
		if (iFailureMode == 4)
		{
			coro::StartTask([iIdParam](coro::Token) -> coro::Task
			{
				OI::Rpc::WakeRpcCall(iIdParam, OI_ERR_FAIL, nullptr);
				co_return;
			});
		}
		throw std::runtime_error("injected submission failure");
	}
};

static bool SameWaitStats(const OI::Rpc::RpcWaitStats& stBaselineParam)
{
	const auto stNow = OI::Rpc::GetRpcWaitStats();
	return stNow.iCalls == stBaselineParam.iCalls && stNow.iExpirations == stBaselineParam.iExpirations && stNow.iBuckets == stBaselineParam.iBuckets && stNow.iSessionEntries == stBaselineParam.iSessionEntries && stNow.iSessionBuckets == stBaselineParam.iSessionBuckets;
}

static int CheckPreconnectSendBudget()
{
	int iBadLocal = 0;
	OI::Base::Bytes stPayload("12345678", 8);
	PendingClient stRawPending;
	stRawPending.SetSendLimit(8);
	if (!stRawPending.SendRawOrPend(stPayload, 1024) || stRawPending.SendRawOrPend(stPayload, 1024))
	{
		++iBadLocal;
	}
	auto pSessionLocal = stRawPending.AttachForTest();
	if (pSessionLocal->PendingSendBytes() != 8 || stRawPending.SendRaw(stPayload))
	{
		++iBadLocal;
	}
	stRawPending.CloseSession(pSessionLocal->GetSessionID());
	pSessionLocal->Release();
	printf("hold preconnect send budget bad=%d\n", iBadLocal);
	return iBadLocal;
}

// 标准回调的可选上下文跨挂起保持有效。
static coro::TaskResult<int> CheckRpcContext(PendingClient& stPendingParam)
{
	using Req = OI::RpcData::HoldReq;
	using Res = OI::RpcData::HoldRes;
	int iBadLocal = 0;
	OI::Base::BytesStream stBody;
	stBody << (uint64_t(1) | uint64_t(RPC_REQ_MASK));
	Req stRequestLocal;
	stRequestLocal.iMark = 73;
	stRequestLocal.serialize(stBody);
	int iCallbacks = 0;
	auto fnContextHandler = [&](const OI::Rpc::RpcContext& stContextParam, const Req& stRequestParam, Res&) -> OI::Rpc::RpcTask
	{
		co_await coro::yield(0);
		if (stContextParam.iToken != 88 || &stContextParam.stManager != &stPendingParam || stContextParam.iSessionId != 77 || stRequestParam.iMark != 73)
		{
			++iBadLocal;
		}
		++iCallbacks;
		co_return 1001;
	};
	co_await OI::Rpc::RunHandledRpc<Req, Res>(fnContextHandler, OI::Rpc::RpcProtoId<Res>::iValue, 88, &stPendingParam, 77, std::move(stBody));
	if (iCallbacks != 1)
	{
		++iBadLocal;
	}
	printf("hold rpc context bad=%d\n", iBadLocal);
	co_return iBadLocal;
}

static uint64_t BindPendingCall(PendingClient& stPendingParam, int32_t* pResultParam, int64_t iTimeoutMillisecondsParam)
{
	return OI::Rpc::BindRpcCall(&stPendingParam, INVALID_SID, 0, {}, pResultParam, iTimeoutMillisecondsParam, {}, []()
	{
	});
}

static int CheckPendingLimits(PendingClient& stPendingParam)
{
	int iBadLocal = 0;
	stPendingParam.SetPendingCallLimits(1, 8);
	OI::Base::BytesStream stFrame;
	stFrame << uint64_t(1);
	int32_t iFirstResult = 0, iSecondResult = 0;
	const auto iFirstCall = BindPendingCall(stPendingParam, &iFirstResult, 0);
	stPendingParam.TryPendCall(iFirstCall, stFrame.GetMutableBuff());
	auto iSecondCall = BindPendingCall(stPendingParam, &iSecondResult, 0);
	stPendingParam.TryPendCall(iSecondCall, stFrame.GetMutableBuff());
	if (iSecondResult != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	OI::Rpc::WakeRpcCall(iFirstCall, OI_ERR_TIMEOUT, nullptr);
	iSecondResult = 0;
	iSecondCall = BindPendingCall(stPendingParam, &iSecondResult, 0);
	stPendingParam.TryPendCall(iSecondCall, stFrame.GetMutableBuff());
	if (iSecondResult != 0)
	{
		++iBadLocal;
	}
	stPendingParam.FailHeldCalls();
	if (iSecondResult != OI_ERR_CONNECT)
	{
		++iBadLocal;
	}
	stPendingParam.SetPendingCallLimits(8, 7);
	iSecondResult = 0;
	iSecondCall = BindPendingCall(stPendingParam, &iSecondResult, 0);
	stPendingParam.TryPendCall(iSecondCall, stFrame.GetMutableBuff());
	if (iSecondResult != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	printf("hold pending limits bad=%d\n", iBadLocal);
	return iBadLocal;
}

// 会话索引换绑后，旧连接断开只能完成仍属于它的调用。
static int CheckRpcSessionIndex(PendingClient& stPendingParam)
{
	int iBadLocal = 0;
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
	std::vector<int32_t> vIndexedResults(4096, -99);
	std::vector<uint64_t> vIndexedCalls;
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		const auto iCallLocal = OI::Rpc::BindRpcCall(&stPendingParam, iIndexLocal < 32 ? 123 : 456, 0, {}, &vIndexedResults[iIndexLocal], 180000, {}, []()
		{
		});
		vIndexedCalls.push_back(iCallLocal);
	}
	for (size_t iIndexLocal = 0; iIndexLocal < 16; ++iIndexLocal)
	{
		if (!OI::Rpc::ClaimRpcSend(vIndexedCalls[iIndexLocal], 789))
		{
			++iBadLocal;
		}
	}
	OI::Rpc::FailRpcCalls(&stPendingParam, 123);
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		if (vIndexedResults[iIndexLocal] != (iIndexLocal >= 16 && iIndexLocal < 32 ? OI_ERR_CONNECT : -99))
		{
			++iBadLocal;
		}
	}
	OI::Rpc::FailRpcCalls(&stPendingParam, 789);
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		if (vIndexedResults[iIndexLocal] != (iIndexLocal < 32 ? OI_ERR_CONNECT : -99))
		{
			++iBadLocal;
		}
		OI::Rpc::WakeRpcCall(vIndexedCalls[iIndexLocal], OI_ERR_CONNECT, nullptr);
	}
	if (!SameWaitStats(stBaseline))
	{
		++iBadLocal;
	}
	printf("hold rpc session index calls=4096 bad=%d\n", iBadLocal);
	return iBadLocal;
}

// 跨分片、跨批次断连清理：回调抛异常也必须完成后续项并归还所有 guard。
static int CheckRpcDisconnectDrain(PendingClient& stPendingParam)
{
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
	std::vector<int32_t> vResults(4096, -99);
	auto pGuard = std::make_shared<std::atomic<int32_t>>(0);
	auto pCallbacks = std::make_shared<std::atomic<int32_t>>(0);
	for (size_t iIndex = 0; iIndex < vResults.size(); ++iIndex)
	{
		OI::Rpc::BindRpcCall(&stPendingParam, 987654, 0, {}, &vResults[iIndex], 180000, {}, [pCallbacks]()
		{
			if (pCallbacks->fetch_add(1) == 0)
			{
				throw std::bad_alloc();
			}
		}, pGuard);
	}
	OI::Rpc::FailRpcCalls(&stPendingParam, 987654);
	int iBad = pCallbacks->load() == 4096 && pGuard->load() == 0 && SameWaitStats(stBaseline) ? 0 : 1;
	for (const auto& iResult : vResults)
	{
		if (iResult != OI_ERR_CONNECT)
		{
			++iBad;
		}
	}
	printf("hold disconnect drain calls=4096 bad=%d\n", iBad);
	return iBad;
}

// 登记后序列化异常，以及发送异常与完成回调竞争；恢复后不能留下悬空等待项。
static coro::TaskResult<int> CheckRpcExceptions(PendingClient& stPendingParam)
{
	int iBadLocal = 0;
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
	struct ThrowingRequest : OI::RpcData::HoldReq
	{
		OI::Base::BytesStream& serialize(OI::Base::BytesStream&) const override
		{
			throw std::runtime_error("injected serialization failure");
		}
	} stThrowingRequest;
	OI::RpcData::HoldReq& stThrowingBase = stThrowingRequest;
	OI::RpcData::HoldRes stUnused;
	int iCaughtErrors = 0;
	try
	{
		co_await OI::Rpc::MakeRpcCallAwaiter(stPendingParam, INVALID_SID, stThrowingBase, stUnused, OI::Timer::Milliseconds(50));
	}
	catch (const std::runtime_error&)
	{
		++iCaughtErrors;
	}
	if (!SameWaitStats(stBaseline))
	{
		++iBadLocal;
	}
	stPendingParam.SetPendingCallLimits(64, 64 * 1024 * 1024);
	OI::RpcData::HoldReq stOrdinary;
	for (int iMode = 1; iMode <= 4; ++iMode)
	{
		stPendingParam.iFailureMode = iMode;
		for (int iIndexLocal = 0; iIndexLocal < 128; ++iIndexLocal)
		{
			try
			{
				co_await OI::Rpc::MakeRpcCallAwaiter(stPendingParam, stOrdinary, stUnused, OI::Timer::Milliseconds(50));
			}
			catch (const std::runtime_error&)
			{
				++iCaughtErrors;
			}
			if (!SameWaitStats(stBaseline))
			{
				++iBadLocal;
			}
		}
	}
	stPendingParam.iFailureMode = 0;
	co_await coro::yield(OI::Timer::Milliseconds(250));
	if (iCaughtErrors != 513 || !SameWaitStats(stBaseline))
	{
		++iBadLocal;
	}
	printf("hold rpc exceptions caught=%d bad=%d\n", iCaughtErrors, iBadLocal);
	co_return iBadLocal;
}

// 每个分片留一条长超时请求堵住队首，再拒绝十万条；索引数量必须保持不变。
static int CheckRpcExpiry(PendingClient& stPendingParam)
{
	int iBadLocal = 0;
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
	OI::Base::BytesStream stFrame;
	stFrame << uint64_t(1);
	stPendingParam.SetPendingCallLimits(64, 64 * 8);
	int32_t vAnchorResults[64]{};
	for (auto& iAnchorResult : vAnchorResults)
	{
		stPendingParam.TryPendCall(BindPendingCall(stPendingParam, &iAnchorResult, 180000), stFrame.GetMutableBuff());
	}
	const auto stAnchored = OI::Rpc::GetRpcWaitStats();
	if (stAnchored.iCalls != stBaseline.iCalls + 64 || stAnchored.iExpirations != stBaseline.iExpirations + 64)
	{
		++iBadLocal;
	}
	for (int iIndexLocal = 0; iIndexLocal < 100000; ++iIndexLocal)
	{
		int32_t iRejected = 0;
		stPendingParam.TryPendCall(BindPendingCall(stPendingParam, &iRejected, 180000 + iIndexLocal % 17), stFrame.GetMutableBuff());
		if (iRejected != OI_ERR_BUSY)
		{
			++iBadLocal;
		}
	}
	const auto stLimited = OI::Rpc::GetRpcWaitStats();
	if (stLimited.iCalls != stAnchored.iCalls || stLimited.iExpirations != stAnchored.iExpirations || stLimited.iBuckets != stAnchored.iBuckets)
	{
		++iBadLocal;
	}
	stPendingParam.FailHeldCalls();
	for (const int32_t iCodeLocal : vAnchorResults)
	{
		if (iCodeLocal != OI_ERR_CONNECT)
		{
			++iBadLocal;
		}
	}
	if (!SameWaitStats(stBaseline))
	{
		++iBadLocal;
	}
	printf("hold rpc expiry rejected=100000 retained=%zu bad=%d\n", stLimited.iExpirations - stBaseline.iExpirations, iBadLocal);
	return iBadLocal;
}

static coro::TaskResult<int> CheckDeadlineBuckets()
{
	int iBad = 0;
	auto& stManager = IpcClient::GetInstance();
	const auto stBefore = OI::Rpc::GetRpcWaitStats();
	std::vector<uint64_t> vCalls;
	const int64_t iTargetMilliseconds = OI::Timer::SteadyMilliseconds() + 60000;
	for (int iIndex = 0; iIndex < 4096; ++iIndex)
	{
		// 不同剩余时长落到同一小段绝对截止时间，而不是产生4096个时长桶。
		const int64_t iRemainingMilliseconds = iTargetMilliseconds + iIndex % 100 - OI::Timer::SteadyMilliseconds();
		vCalls.push_back(OI::Rpc::BindRpcCall(&stManager, INVALID_SID, 0, {}, nullptr, iRemainingMilliseconds, {}, {}));
	}
	const auto stDuring = OI::Rpc::GetRpcWaitStats();
	if (stDuring.iCalls != stBefore.iCalls + 4096 || stDuring.iBuckets > stBefore.iBuckets + 192)
	{
		++iBad;
	}
	vCalls.push_back(OI::Rpc::BindRpcCall(&stManager, INVALID_SID, 0, {}, nullptr, INT64_MAX, {}, {}));
	for (const auto& iCall : vCalls)
	{
		OI::Rpc::AbandonRpcCall(iCall);
	}
	vCalls.clear();
	struct Probe
	{
		std::atomic<int> iCompleted{0};
		std::atomic<int> iBad{0};
		std::vector<int32_t> vCodes = std::vector<int32_t>(32, 0);
	};
	auto pProbe = std::make_shared<Probe>();
	auto pGuard = std::make_shared<std::atomic<int32_t>>(0);
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		const int64_t iDelayMilliseconds = 50 + (31 - iIndex) * 23;
		const int64_t iDeadlineMilliseconds = OI::Timer::SteadyMilliseconds() + iDelayMilliseconds;
		vCalls.push_back(OI::Rpc::BindRpcCall(&stManager, INVALID_SID, 0, {}, &pProbe->vCodes[iIndex], iDelayMilliseconds, {},
		    [pProbe, iIndex, iDeadlineMilliseconds]()
		{
			if (OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds || pProbe->vCodes[iIndex] != OI_ERR_TIMEOUT)
			{
				++pProbe->iBad;
			}
			++pProbe->iCompleted;
		}, pGuard));
	}
	const int64_t iLimitMilliseconds = OI::Timer::SteadyMilliseconds() + 4000;
	while (pProbe->iCompleted.load() != 32 && OI::Timer::SteadyMilliseconds() < iLimitMilliseconds)
	{
		co_await coro::yield(20);
	}
	for (const auto& iCall : vCalls)
	{
		OI::Rpc::AbandonRpcCall(iCall);
	}
	OI::Rpc::WaitRpcGuard(pGuard);
	if (pProbe->iCompleted.load() != 32 || pProbe->iBad.load() != 0 || !SameWaitStats(stBefore))
	{
		++iBad;
	}
	printf("hold absolute deadline buckets calls=4096 buckets=%zu fired=%d bad=%d\n", stDuring.iBuckets - stBefore.iBuckets, pProbe->iCompleted.load(), iBad);
	fflush(stdout);
	co_return iBad;
}

// 只在内部配对入口注入错误来源，不构造另一套网络客户端。
static int CheckRpcSource(sid_t iSessionIdParam)
{
	int iBadLocal = 0;
	auto& stManagerLocal = IpcClient::GetInstance();
	const auto iResponseIdLocal = ::OI::Rpc::RpcProtoId<::OI::RpcData::HoldRes>::iValue;
	int iDecoded = 0, iResumed = 0, iResultLocal = -99;
	uint64_t iCall = ::OI::Rpc::BindRpcCall(&stManagerLocal, iSessionIdParam, iResponseIdLocal, {}, &iResultLocal, 0, [pDecoded = &iDecoded](::OI::Base::BytesStream&, sid_t)
	{
		++*pDecoded;
		return true;
	}, [&]()
	{
		++iResumed;
	});
	auto fnDeliver = [&](::OI::Net::SessionManager* pSourceParam, sid_t iSourceSessionParam)
	{
		::OI::Base::BytesStream stBody;
		stBody << iCall;
		::OI::Rpc::DeliverRpc(iResponseIdLocal, stBody, pSourceParam, iSourceSessionParam);
	};
	fnDeliver(&stManagerLocal, iSessionIdParam + 100000);
	fnDeliver(&::OI::Redis::RedisClient::GetInstance(), iSessionIdParam);
	if (iDecoded || iResumed)
	{
		++iBadLocal;
	}
	fnDeliver(&stManagerLocal, iSessionIdParam);
	if (iDecoded != 1 || iResumed != 1)
	{
		++iBadLocal;
	}
	iResumed = 0;
	const sid_t iFakeSessionId = iSessionIdParam + 100000;
	::OI::Rpc::BindRpcCall(&stManagerLocal, iFakeSessionId, iResponseIdLocal, {}, &iResultLocal, 0, {}, [&]()
	{
		++iResumed;
	});
	::OI::Rpc::FailRpcCalls(&stManagerLocal, iFakeSessionId);
	::OI::Rpc::FailRpcCalls(&stManagerLocal, iFakeSessionId);
	if (iResultLocal != OI_ERR_CONNECT || iResumed != 1)
	{
		++iBadLocal;
	}
	printf("hold rpc source and disconnect bad=%d\n", iBadLocal);
	return iBadLocal;
}

static coro::TaskResult<int> CheckStandardizedReply()
{
	int iBadLocal = 0;
	auto& stManagerLocal = IpcClient::GetInstance();
	::OI::RpcData::HoldReq stRequestLocal;
	::OI::RpcData::HoldRes stResponseLocal;
	stRequestLocal.iDelayMilliseconds = 0;
	stRequestLocal.iMark = 1;
	auto stDirect = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
	if (!stDirect || stDirect.GetRes().iMark != 1)
	{
		++iBadLocal;
	}
	static_assert(std::is_same_v<decltype(stDirect.GetRet()), int32_t>);
	stRequestLocal.iDelayMilliseconds = -1;
	auto stFailed = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal);
	if (stFailed.GetRet() != 1001 || stFailed)
	{
		++iBadLocal;
	}
	bool bGuarded = false;
	try
	{
		(void)stFailed.GetRes();
	}
	catch (const std::logic_error&)
	{
		bGuarded = true;
	}
	if (!bGuarded)
	{
		++iBadLocal;
	}
	stResponseLocal.iMark = 876;
	int32_t iBusinessReturnCode = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, stResponseLocal);
	if (iBusinessReturnCode != 1001 || stResponseLocal.iMark != 876)
	{
		++iBadLocal;
	}
	stRequestLocal.iDelayMilliseconds = 0;
	iBusinessReturnCode = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, stResponseLocal, ::OI::Timer::Seconds(10));
	if (iBusinessReturnCode != 0 || stResponseLocal.iMark != 1)
	{
		++iBadLocal;
	}
	// 缺状态码和截断响应都应返回框架错误，不能覆盖应用已有响应。
	for (int iMalformed = 0; iMalformed < 2; ++iMalformed)
	{
		stResponseLocal.iMark = 876;
		auto stBroken = ::OI::Rpc::MakeRpcCallAwaiter(stManagerLocal, stRequestLocal, stResponseLocal);
		stBroken.bReplied = true;
		if (iMalformed)
		{
			stBroken.stReply << int32_t(0) << uint8_t(1);
		}
		if (stBroken.await_resume() != OI_ERR_FAIL || stResponseLocal.iMark != 876)
		{
			++iBadLocal;
		}
	}
	auto fnValueTask = []() -> coro::TaskResult<int32_t>
	{
		co_await coro::yield(0);
		co_return 77;
	};
	if (co_await fnValueTask() != 77)
	{
		++iBadLocal;
	}
	auto fnThrowingTask = []() -> coro::TaskResult<int32_t>
	{
		co_await coro::yield(0);
		throw std::runtime_error("result task test");
		co_return 0;
	};
	bool bPropagated = false;
	try
	{
		(void)co_await fnThrowingTask();
	}
	catch (const std::runtime_error&)
	{
		bPropagated = true;
	}
	if (!bPropagated)
	{
		++iBadLocal;
	}
	printf("hold rpc standardized reply bad=%d\n", iBadLocal);
	co_return iBadLocal;
}

// 超时后迟到的回包只能丢弃，不能配给后面的调用。
static coro::TaskResult<int> CheckRpcLateReply()
{
	int iBadLocal = 0;
	::OI::RpcData::HoldReq stRequestLocal;
	::OI::RpcData::HoldRes stResponseLocal;
	stRequestLocal.iMark = 1;
	stRequestLocal.iDelayMilliseconds = 600;
	auto stShortWait = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Milliseconds(150));
	if (stShortWait.GetRet() != OI_ERR_TIMEOUT)
	{
		++iBadLocal;
	}
	co_await coro::yield(::OI::Timer::Milliseconds(700));
	stRequestLocal.iDelayMilliseconds = 0;
	auto stEarly = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
	int iReturnCodeLocal = stEarly.GetRet();
	if (iReturnCodeLocal == 0)
	{
		stResponseLocal = stEarly.GetRes();
	}
	printf("hold rpc early ret=%d mark=%d\n", iReturnCodeLocal, stResponseLocal.iMark);
	fflush(stdout);
	if (iReturnCodeLocal != 0 || stResponseLocal.iMark != 1)
	{
		iBadLocal += 1;
	}

	std::clock_t iCpuBegin = std::clock();
	int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	stRequestLocal.iDelayMilliseconds = 1500;
	stRequestLocal.iMark = 2;
	stResponseLocal.iMark = 0;
	auto stTimeout = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Milliseconds(500));
	iReturnCodeLocal = stTimeout.GetRet();
	printf("hold rpc timeout ret=%d\n", iReturnCodeLocal);
	fflush(stdout);
	if (iReturnCodeLocal != OI_ERR_TIMEOUT)
	{
		iBadLocal += 1;
	}
	// 回包还在路上。让这一下把迟到的包和已经武装的超时定时器都消化掉。
	co_await coro::yield(1500);
	printf("hold rpc late-reply cpu_ms=%lld wall_ms=%lld\n",
	    CpuMs(iCpuBegin), WallMs(iWallBegin));
	fflush(stdout);

	stRequestLocal.iDelayMilliseconds = 0;
	stRequestLocal.iMark = 3;
	stResponseLocal.iMark = 0;
	auto stFollow = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
	iReturnCodeLocal = stFollow.GetRet();
	stResponseLocal.iMark = iReturnCodeLocal == 0 ? stFollow.GetRes().iMark : 0;
	printf("hold rpc follow ret=%d mark=%d\n", iReturnCodeLocal, stResponseLocal.iMark);
	fflush(stdout);
	if (iReturnCodeLocal != 0 || stResponseLocal.iMark != 3)
	{
		iBadLocal += 1;
	}
	co_return iBadLocal;
}

// 服务端延迟落在客户端超时前后，每个调用只能拿到回包或超时之一。
static coro::TaskResult<int> CheckRpcRace()
{
	const int vDelays[] = {800, 900, 1000, 1100, 1200, 1400, 1600, 1800};
	const std::clock_t iCpuBegin = std::clock();
	const int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	std::vector<coro::TaskResult<RaceOutcome>> vCalls;
	for (int iIndexLocal = 0; iIndexLocal < 8; ++iIndexLocal)
	{
		vCalls.push_back(RaceOne(100 + iIndexLocal, vDelays[iIndexLocal]));
	}
	int iReplyNumber = 0, iTimeoutNumber = 0, iRaceBad = 0;
	for (const auto& stCall : vCalls)
	{
		const RaceOutcome iOutcome = co_await stCall;
		iReplyNumber += iOutcome == RaceOutcome::Reply ? 1 : 0;
		iTimeoutNumber += iOutcome == RaceOutcome::Timeout ? 1 : 0;
		iRaceBad += iOutcome == RaceOutcome::Bad ? 1 : 0;
	}
	printf("hold rpc race n=%zu done=%zu reply=%d timeout=%d bad=%d cpu_ms=%lld wall_ms=%lld\n",
	    vCalls.size(), vCalls.size(), iReplyNumber, iTimeoutNumber, iRaceBad, CpuMs(iCpuBegin), WallMs(iWallBegin));
	fflush(stdout);
	co_return iRaceBad;
}

static coro::TaskResult<int> CheckRedisPing(const char* pLabelParam)
{
	const int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	const auto stPing = co_await ::OI::Redis::CallRedis({"PING"});
	printf("hold redis %sping ok=%d status=%d reply_type=%d wall_ms=%lld\n", pLabelParam, Pong(stPing) ? 1 : 0,
	    static_cast<int>(stPing.iStatus), stPing.pReply ? stPing.pReply->type : -1, WallMs(iWallBegin));
	fflush(stdout);
	co_return Pong(stPing) ? 0 : 1;
}

// Redis 超时后迟到的回复不能串给下一条命令；回复与超时同时到也只能取其一。
static coro::TaskResult<int> CheckRedisLateReply(int32_t iRedisTimeoutMillisecondsParam)
{
	int iSpins = 0;
	int iLimit = iRedisTimeoutMillisecondsParam > 0 ? iRedisTimeoutMillisecondsParam / 50 : 100;
	if (iLimit < 1)
	{
		iLimit = 1;
	}
	while (!::OI::Redis::RedisClient::GetInstance().IsConnect())
	{
		if (iSpins++ >= iLimit)
		{
			printf("hold redis connect fail\n");
			fflush(stdout);
			co_return 1;
		}
		co_await coro::yield(50);
	}
	int iBadLocal = co_await CheckRedisPing("");
	int iSeconds = iRedisTimeoutMillisecondsParam / 1000;
	if (iSeconds < 1)
	{
		iSeconds = 1;
	}

	std::clock_t iCpuBegin = std::clock();
	int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	std::vector<std::string> vLate{"BLPOP", "hold-late", std::to_string(iSeconds + 1)};
	const auto stLate = co_await ::OI::Redis::CallRedis(std::move(vLate));
	printf("hold redis late timeout=%d cpu_ms=%lld wall_ms=%lld\n",
	    stLate.bTimeout ? 1 : 0, CpuMs(iCpuBegin), WallMs(iWallBegin));
	fflush(stdout);
	if (!stLate.bTimeout)
	{
		iBadLocal += 1;
	}
	iBadLocal += co_await CheckRedisPing("late ");

	iCpuBegin = std::clock();
	iWallBegin = ::OI::Timer::SteadyMilliseconds();
	std::vector<std::string> vRace{"BLPOP", "hold-race", std::to_string(iSeconds)};
	const auto stRace = co_await ::OI::Redis::CallRedis(std::move(vRace));
	const int iNil = stRace.pReply && stRace.pReply->type == REDIS_REPLY_NIL ? 1 : 0;
	printf("hold redis race timeout=%d nil=%d cpu_ms=%lld wall_ms=%lld\n",
	    stRace.bTimeout ? 1 : 0, iNil, CpuMs(iCpuBegin), WallMs(iWallBegin));
	fflush(stdout);
	if (!stRace.bTimeout && !iNil)
	{
		iBadLocal += 1;
	}
	iBadLocal += co_await CheckRedisPing("race ");
	co_return iBadLocal;
}

static coro::Task CloseSessionLater(sid_t iSessionIdParam)
{
	co_await coro::yield(::OI::Timer::Milliseconds(100));
	IpcClient::GetInstance().CloseSession(iSessionIdParam);
}

// 真断开当前会话：已发出的 RPC 应及时失败，不能继续等十秒超时。
static coro::TaskResult<int> CheckRpcDisconnect(sid_t iSessionIdParam)
{
	::OI::RpcData::HoldReq stRequestLocal;
	stRequestLocal.iDelayMilliseconds = 5000;
	stRequestLocal.iMark = 4;
	auto stClose = CloseSessionLater(iSessionIdParam);
	const int64_t iWallBegin = ::OI::Timer::SteadyMilliseconds();
	auto stClosed = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
	const long long iWallMilliseconds = WallMs(iWallBegin);
	co_await stClose;
	printf("hold rpc disconnected code=%d wall_ms=%lld\n", stClosed.GetRet(), iWallMilliseconds);
	co_return (stClosed.GetRet() != OI_ERR_CONNECT || iWallMilliseconds > 2000) ? 1 : 0;
}

coro::Task HoldMgr::Pump()
{
	bool bAdmission = false;
	TryGetConfig("Regression.rpc_admission", bAdmission);
	if (bAdmission)
	{
		const int iBad = co_await CheckRpcAdmission();
		TestBoard::GetInstance().Done(iBad ? 1 : 0);
		co_return;
	}
	int iBad = co_await CheckTimerExceptions();
	if (iBad != 0)
	{
		printf("hold done bad=%d\n", iBad);
		fflush(stdout);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	iBad += co_await CheckNestedResults();
	iBad += co_await CheckStartedTaskWaiters();
	iBad += co_await CheckHotStart();
	iBad += co_await CheckSharedTasks();
	iBad += co_await CheckDeferredRpcSession();
	iBad += co_await CheckOutputRpcTask(m_iSessionId);
	iBad += co_await CheckAwaitOwnership();
	iBad += CheckBinaryBool();
	iBad += co_await CheckClientConnect();
	iBad += co_await CheckDirectBackpressure();
	iBad += co_await CheckConnectionIsolation();
	bool bRegistryChecks = false;
	TryGetConfig("Regression.registry_checks", bRegistryChecks);
	if (bRegistryChecks)
	{
		iBad += co_await CheckRegistryPairing();
		iBad += co_await CheckNamedServiceRpc();
		iBad += co_await CheckNamedOutputRpcTask();
		iBad += co_await CheckCompletedRpcRequests(m_iSessionId);
		iBad += co_await CheckRoutePoolLifetime();
		iBad += co_await CheckRouteInboundAndReconnect();
		iBad += co_await CheckRoutePoolCapacity();
		iBad += co_await CheckRegistryBoundsAndBudget();
		iBad += co_await CheckRouteTargetStability();
	}
	iBad += CheckJsonDecoding();
	iBad += co_await CheckTaskExceptions();
	bool bEncodingRegression = false;
	TryGetConfig("Regression.encoding_failure", bEncodingRegression);
	if (bEncodingRegression)
	{
		iBad += co_await CheckServerEncoding();
	}
	int32_t iHttpPort = 0;
	TryGetConfig("Regression.http_port", iHttpPort);
	if (iHttpPort > 0)
	{
		iBad += co_await CheckHttpFraming(iHttpPort);
		iBad += co_await CheckHttpRelease(iHttpPort);
	}
	iBad += CheckSendBudget();
	iBad += co_await CheckSchedulerFairness();
	iBad += co_await CheckTaskScope();
	iBad += co_await CheckCapturedTasks();
	iBad += CheckPreconnectSendBudget();
	PendingClient stPending;
	iBad += co_await CheckRpcContext(stPending);
	iBad += CheckPendingLimits(stPending);
	iBad += CheckRpcSessionIndex(stPending);
	iBad += CheckRpcDisconnectDrain(stPending);
	iBad += co_await CheckRpcExceptions(stPending);
	iBad += CheckRpcExpiry(stPending);
	iBad += co_await CheckDeadlineBuckets();
	iBad += CheckRpcSource(m_iSessionId);
	iBad += co_await CheckStandardizedReply();
	iBad += co_await CheckRpcLateReply();
	iBad += co_await CheckRpcRace();
	iBad += co_await CheckRedisLateReply(m_iRedisTimeoutMilliseconds);
	iBad += co_await CheckRpcDisconnect(m_iSessionId);
	printf("hold done bad=%d\n", iBad);
	fflush(stdout);
	TestBoard::GetInstance().Done(iBad == 0 ? 0 : 1);
}
