#include "hold_mgr.h"
#include "oi_http.h"
#include "oi_log_client.h"
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
#include <memory>
#include <optional>
#include <string>
#include <vector>

HoldMgr::HoldMgr()
    : m_bEnabled(false), m_iSessionId(0), m_iRedisTimeoutMilliseconds(25000)
{
}

void HoldMgr::Load()
{
	TryGetConfig("Test.hold", m_bEnabled);
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

struct RaceBox
{
	int32_t vReturnCodes[8];
	int32_t vMarks[8];
	std::atomic<int32_t> iFinished;
	RaceBox()
	    : iFinished(0)
	{
		for (int iIndexLocal = 0; iIndexLocal < 8; ++iIndexLocal)
		{
			vReturnCodes[iIndexLocal] = -99;
			vMarks[iIndexLocal] = 0;
		}
	}
};

static coro::Task RaceOne(int32_t iIndexParam, int32_t iMarkParam, int32_t iDelayParam, std::shared_ptr<RaceBox> pBoxParam)
{
	::OI::RpcData::HoldReq stOne;
	stOne.iDelayMilliseconds = iDelayParam;
	stOne.iMark = iMarkParam;
	auto stResult = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stOne, ::OI::Timer::Seconds(10));
	pBoxParam->vReturnCodes[iIndexParam] = stResult.GetRet();
	pBoxParam->vMarks[iIndexParam] = stResult.GetRet() == 0 ? stResult.GetRes().iMark : 0;
	pBoxParam->iFinished.fetch_add(1, std::memory_order_release);
	co_return;
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

static coro::TaskResult<int> CheckClientConnect()
{
	static ClientConnectProbe stGood("17981"), stRefused("0");
	int iBad = 0;
	static ClientConnectProbe stEager("17981"), stDelayedAwait("17981");
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
	static ClientConnectProbe stAwait("17981"), stAwaitRefused("0");
	coro::TaskScope stConnectTasks;
	auto pBad = std::make_shared<std::atomic<int>>(0);
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		stConnectTasks.Start([pBad](coro::Token, coro::StopToken) -> coro::Task
		{
			if (!(co_await stAwait.StartClient()))
			{
				++*pBad;
			}
		});
	}
	co_await stConnectTasks.Join();
	if (stAwait.Connections() != 1 || !(co_await stAwait.StartClient()))
	{
		++iBad;
	}
	if (co_await stAwaitRefused.StartClient())
	{
		++iBad;
	}
	static ClientConnectProbe stReentrant("17981", true);
	const bool bFirst = co_await stReentrant.StartClient();
	const bool bRetry = co_await stReentrant.Retry();
	const int iReentrantBad = (!bFirst || bRetry || stReentrant.Connections() != 1) ? 1 : 0;
	iBad += iReentrantBad;
	printf("hold reentrant connect first=%d retry=%d connections=%d bad=%d\n", bFirst, bRetry, stReentrant.Connections(), iReentrantBad);
	stAwait.CloseAllSessions();
	stGood.CloseAllSessions();
	co_return iBad + pResult->iBad + pBad->load();
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

static coro::TaskResult<int> CheckNestedResults()
{
	struct Results
	{
		std::atomic<int> iCompleted{0};
		std::atomic<int> iCaught{0};
		std::atomic<int> iBad{0};
	};
	auto pResults = std::make_shared<Results>();
	coro::TaskScope stTasks;
	for (int iWorker = 0; iWorker < 64; ++iWorker)
	{
		stTasks.Start([pResults, iWorker](coro::Token, coro::StopToken) -> coro::Task
		{
			for (int iIndex = 0; iIndex < 512; ++iIndex)
			{
				const int iValue = iWorker * 512 + iIndex;
				try
				{
					const int iResult = co_await ResultStressParent(iValue);
					if (iResult != iValue + 1 || (iValue % 8 >= 2 && iValue % 8 <= 5))
					{
						++pResults->iBad;
					}
				}
				catch (const std::runtime_error& stError)
				{
					++pResults->iCaught;
					if ((iValue % 8 != 2 && iValue % 8 != 3) || std::strcmp(stError.what(), "hold nested result exception") != 0)
					{
						++pResults->iBad;
					}
				}
				catch (int iError)
				{
					++pResults->iCaught;
					if ((iValue % 8 != 4 && iValue % 8 != 5) || iError != iValue)
					{
						++pResults->iBad;
					}
				}
				catch (...)
				{
					++pResults->iBad;
				}
				++pResults->iCompleted;
			}
		});
	}
	co_await stTasks.Join();
	if (pResults->iCompleted != 32768 || pResults->iCaught != 16384)
	{
		++pResults->iBad;
	}
	printf("hold nested results completed=%d caught=%d bad=%d\n", pResults->iCompleted.load(), pResults->iCaught.load(), pResults->iBad.load());
	fflush(stdout);
	co_return pResults->iBad.load();
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
					vWaiters.push_back(coro::StartTask([stTask, pState, iIndex](coro::Token) -> coro::Task
					{
						const int iResult = co_await stTask;
						if (iResult != 42 || pState->vHits[iIndex].fetch_add(1) != 0)
						{
							++pState->iBad;
						}
						++pState->iAwakened;
					}));
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
static coro::TaskResult<int> CheckSharedTasks()
{
	auto pBad = std::make_shared<std::atomic<int>>(0);
	for (int iMode = 0; iMode < 2; ++iMode)
	{
		auto pRuns = std::make_shared<std::atomic<int>>(0);
		auto stTask = SharedResultChild(pRuns, iMode != 0);
		coro::TaskScope stWaiters;
		for (int iIndex = 0; iIndex < 64; ++iIndex)
		{
			stWaiters.Start([stTask, pBad, iMode](coro::Token, coro::StopToken) -> coro::Task
			{
				// 第二遍是完成后再次等待，不能重新执行，也不能拿到被移空的字符串。
				for (int iRound = 0; iRound < 2; ++iRound)
				{
					try
					{
						auto sResult = co_await stTask;
						if (iMode != 0 || sResult != std::string(256, 's'))
						{
							++*pBad;
						}
					}
					catch (const std::runtime_error& stError)
					{
						if (iMode == 0 || std::string(stError.what()) != "shared task failure")
						{
							++*pBad;
						}
					}
				}
			});
		}
		co_await stWaiters.Join();
		if (pRuns->load() != 1)
		{
			++*pBad;
		}
	}
	auto pRuns = std::make_shared<std::atomic<int>>(0);
	auto stTask = SharedVoidChild(pRuns);
	coro::TaskScope stStarters;
	for (int iIndex = 0; iIndex < 64; ++iIndex)
	{
		stStarters.Start([stTask](coro::Token, coro::StopToken) -> coro::Task
		{
			stTask.Start();
			stTask.Start();
			co_await stTask;
			co_await stTask;
		});
	}
	co_await stStarters.Join();
	if (pRuns->load() != 1)
	{
		++*pBad;
	}
	// 等待器独立保活：创建等待器后移动/销毁原 Task 仍安全。
	auto pLifetimeRuns = std::make_shared<std::atomic<int>>(0);
	auto pLifetimeTask = std::make_unique<coro::TaskResult<std::string>>(SharedResultChild(pLifetimeRuns, false));
	auto stWaiter = pLifetimeTask->operator co_await();
	pLifetimeTask.reset();
	if ((co_await stWaiter) != std::string(256, 's') || pLifetimeRuns->load() != 1)
	{
		++*pBad;
	}
	auto stSource = SharedVoidChild(pLifetimeRuns);
	auto stMoved = std::move(stSource);
	try
	{
		co_await stSource;
		++*pBad;
	}
	catch (const std::logic_error&)
	{
	}
	try
	{
		stSource.Start();
		++*pBad;
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
	auto pUnusedRuns = std::make_shared<std::atomic<int>>(0);
	auto stReplaced = SharedVoidChild(pUnusedRuns);
	stReplaced = std::move(stCopy);
	co_await stReplaced;
	if (pLifetimeRuns->load() != 2 || pUnusedRuns->load() != 0)
	{
		++*pBad;
	}
	printf("hold shared tasks waiters=64 bad=%d\n", pBad->load());
	co_return pBad->load();
}

class DeferredRpcProbe : public OI::Rpc::ProtoClientManager
{
  public:
	DeferredRpcProbe()
	{
		DefaultInit("127.0.0.1", "17981");
	}
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
	}
};

template <typename T>
static coro::TaskResult<int> CheckSingleUse(std::shared_ptr<T> pCallParam)
{
	struct Counts
	{
		std::atomic<int> iAccepted{0};
		std::atomic<int> iRejected{0};
	};
	std::shared_ptr<Counts> pCounts = std::make_shared<Counts>();
	coro::TaskScope stTasks;
	for (int iIndex = 0; iIndex < 2; ++iIndex)
	{
		stTasks.Start([pCallParam, pCounts](coro::Token, coro::StopToken) -> coro::Task
		{
			try
			{
				co_await *pCallParam;
				++pCounts->iAccepted;
			}
			catch (const std::logic_error&)
			{
				++pCounts->iRejected;
			}
		});
	}
	co_await stTasks.Join();
	try
	{
		co_await *pCallParam;
		++pCounts->iAccepted;
	}
	catch (const std::logic_error&)
	{
		++pCounts->iRejected;
	}
	co_return pCounts->iAccepted == 1 && pCounts->iRejected == 2 ? 0 : 1;
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
	auto stOutputWait = OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stOutput, OI::Timer::Seconds(3));
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
	auto pStarted = std::make_shared<std::atomic<int>>(0);
	auto pBad = std::make_shared<std::atomic<int>>(0);
	coro::TaskScope stTasks;
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		stTasks.Start([pConnect, pStarted, pBad](coro::Token, coro::StopToken) -> coro::Task
		{
			++*pStarted;
			if (co_await *pConnect)
			{
				++*pBad;
			}
		});
	}
	while (pStarted->load() != 32)
	{
		co_await coro::yield(0);
	}
	OI::Net::Poll::GetInstance().CallOnLoop([](void*)
	{
		stClient.Finish();
	}, nullptr);
	co_await stTasks.Join();
	if (co_await *pConnect)
	{
		++iBad;
	}
	iBad += pBad->load();
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

static coro::TaskResult<int> CheckRpcAdmission()
{
	struct Results
	{
		std::atomic<int> iOk{0};
		std::atomic<int> iBusy{0};
		std::atomic<int> iBad{0};
		std::atomic<int> vAccepted[3]{};
	};
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
		auto pResults = std::make_shared<Results>();
		coro::TaskScope stTasks;
		for (int iClient = 0; iClient < 3; ++iClient)
		{
			for (int iIndex = 0; iIndex < 2; ++iIndex)
			{
				stTasks.Start([pResults, iClient](coro::Token, coro::StopToken) -> coro::Task
				{
					OI::RpcData::HoldReq stRequest;
					stRequest.iDelayMilliseconds = 1000;
					stRequest.iMark = 71;
					auto stResult = co_await OI::Rpc::RpcCall(vClients[iClient], stRequest, OI::Timer::Seconds(5));
					if (stResult.GetRet() == 0 && stResult.GetRes().iMark == 71)
					{
						++pResults->iOk;
						++pResults->vAccepted[iClient];
					}
					else if (stResult.GetRet() == OI_ERR_BUSY)
					{
						++pResults->iBusy;
					}
					else
					{
						++pResults->iBad;
					}
				});
			}
		}
		co_await stTasks.Join();
		if (pResults->iOk != 2 || pResults->iBusy != 4 || pResults->iBad != 0)
		{
			++iBad;
		}
		for (const auto& iAccepted : pResults->vAccepted)
		{
			if (iAccepted.load() > 1)
			{
				++iBad;
			}
		}
		printf("hold rpc admission round=%d ok=%d busy=%d bad=%d\n", iRound, pResults->iOk.load(), pResults->iBusy.load(), iBad);
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
		if ((co_await OI::Rpc::RpcCall(vClients[0], stBase, stOutput, OI::Timer::Seconds(3))) != OI_ERR_BUSY)
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
		if (iPinnedCode != OI_ERR_FAIL)
		{
			++iBad;
		}
	}
	stClient.CloseAllSessions();
	printf("hold deferred rpc value=%d output=%d pinned=%d bad=%d\n", iValueCode, iOutputCode, iPinnedCode, iBad);
	fflush(stdout);
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
	coro::TaskScope stTasks;
	auto pBad = std::make_shared<std::atomic<int>>(0);
	for (int iIndex = 0; iIndex < 32; ++iIndex)
	{
		stTasks.Start([iIndex, pBad](coro::Token, coro::StopToken) -> coro::Task
		{
			IpcClient::GetInstance().StartClient();
			OI::Redis::RedisClient::GetInstance().StartClient();
			OI::Log::LogClient::GetInstance().StartClient();
			OI::RpcData::HoldReq stRequest;
			stRequest.iMark = 10000 + iIndex;
			auto stResult = co_await IpcClient::GetInstance().CallAny<OI::RpcData::HoldRes>(stRequest, OI::Timer::Seconds(3));
			if (stResult.GetRet() != 0 || stResult.GetRes().iMark != stRequest.iMark)
			{
				++*pBad;
			}
			std::vector<std::string> vArguments{"ECHO", std::to_string(iIndex)};
			auto stReply = co_await OI::Redis::CallRedis(std::move(vArguments));
			if (!stReply || !stReply.pReply->str || std::string(stReply.pReply->str, stReply.pReply->len) != std::to_string(iIndex))
			{
				++*pBad;
			}
		});
	}
	co_await stTasks.Join();
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
	if (!Pong(co_await OI::Redis::RedisPing()))
	{
		++iBad;
	}
	co_return iBad + pBad->load() + pProbe->iBad;
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
	if (iMany != 2 || iSlot != 1 || vEmptyMany.size() != 1 || vEmptyMany[0].GetRet() != OI_ERR_FAIL || vGlobalMany.size() != 1 || vGlobalMany[0].GetRet() != OI_ERR_FAIL)
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
	auto stFallback = OI::Rpc::RouteService(&stOfflineWatcher, "ipcserver", OI::Rpc::ServicePick::Any, "");
	if (stFallback.iStatus != OI::Rpc::ServiceRouteStatus::Ok || stFallback.vClients.size() != 1 || stFallback.vClients[0] == &stOfflineWatcher)
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
	auto pSingle = std::make_shared<decltype(stSingle)>(std::move(stSingle));
	auto pSuccess = std::make_shared<std::atomic<int>>(0);
	auto pRejected = std::make_shared<std::atomic<int>>(0);
	coro::TaskScope stContenders;
	for (int iIndex = 0; iIndex < 8; ++iIndex)
	{
		stContenders.Start([pSingle, pSuccess, pRejected](coro::Token, coro::StopToken) -> coro::Task
		{
			try
			{
				auto stReply = co_await *pSingle;
				if (stReply.GetRet() == 0 && stReply.GetRes().iMark == 77)
				{
					++*pSuccess;
				}
			}
			catch (const std::logic_error&)
			{
				++*pRejected;
			}
		});
	}
	co_await stContenders.Join();
	try
	{
		(void)co_await *pSingle;
		++iBad;
	}
	catch (const std::logic_error&)
	{
	}
	if (pSuccess->load() != 1 || pRejected->load() != 7)
	{
		++iBad;
	}
	printf("hold named rpc ownership success=%d rejected=%d bad=%d\n", pSuccess->load(), pRejected->load(), iBad);
	fflush(stdout);
	co_return iBad;
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
	int iBadLocal = co_await CheckTimerExceptions();
	if (iBadLocal != 0)
	{
		printf("hold done bad=%d\n", iBadLocal);
		fflush(stdout);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	iBadLocal += co_await CheckNestedResults();
	iBadLocal += co_await CheckStartedTaskWaiters();
	iBadLocal += co_await CheckSharedTasks();
	iBadLocal += co_await CheckDeferredRpcSession();
	iBadLocal += co_await CheckAwaitOwnership();
	iBadLocal += CheckBinaryBool();
	printf("hold binary bool bad=%d\n", iBadLocal);
	iBadLocal += co_await CheckClientConnect();
	printf("hold coroutine connect bad=%d\n", iBadLocal);
	iBadLocal += co_await CheckConnectionIsolation();
	printf("hold connection isolation bad=%d\n", iBadLocal);
	bool bRegistryChecks = false;
	TryGetConfig("Regression.registry_checks", bRegistryChecks);
	if (bRegistryChecks)
	{
		coro::TaskScope stLookups;
		auto pLookupBad = std::make_shared<std::atomic<int>>(0);
		for (int iIndex = 0; iIndex < 8; ++iIndex)
		{
			stLookups.Start([pLookupBad](coro::Token, coro::StopToken) -> coro::Task
			{
				for (int iRound = 0; iRound < 8; ++iRound)
				{
					// 周期拉取与同步查询交错，队列顺序必须和线上顺序一致。
					OI::Base::RegistryClient::GetInstance().Pull();
					std::string sIp, sPort;
					if (!OI::Base::RegistryClient::GetInstance().Lookup("ipcserver", sIp, sPort) || sIp != "127.0.0.1" || sPort != "17981")
					{
						++*pLookupBad;
					}
					co_await coro::yield(0);
				}
			});
		}
		co_await stLookups.Join();
		iBadLocal += pLookupBad->load();
		printf("hold registry pairing bad=%d\n", iBadLocal);
		iBadLocal += co_await CheckNamedServiceRpc();
	}

	iBadLocal += CheckJsonDecoding();
	printf("hold json validation bad=%d\n", iBadLocal);
	// 异常必须释放闭包；随后继续执行普通任务，不能终止工作线程。
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

	bool bEncodingRegression = false;
	TryGetConfig("Regression.encoding_failure", bEncodingRegression);
	if (bEncodingRegression)
	{
		OI::RpcData::HoldReq stRequest;
		stRequest.iMark = -2147483600;
		OI::RpcData::HoldRes stResponse;
		stResponse.iMark = 91;
		int32_t iResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
		if (iResult != OI_ERR_FAIL || stResponse.iMark != 91)
		{
			++iBadLocal;
		}
		stRequest.iMark = 92;
		auto stRecovered = co_await IpcClient::GetInstance().CallAny<OI::RpcData::HoldRes>(stRequest, OI::Timer::Seconds(2));
		if (stRecovered.GetRet() != 0 || stRecovered.GetRes().iMark != 92)
		{
			++iBadLocal;
		}
		printf("hold server encoding bad=%d\n", iBadLocal);
	}
	int32_t iHttpPort = 0;
	TryGetConfig("Regression.http_port", iHttpPort);
	if (iHttpPort > 0)
	{
		OI::Http::Client stHttp("127.0.0.1", iHttpPort, 1000);
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
		// 长超时期间连续成功的大响应，不应把已完成的收包容量留到超时。
		auto fnAllocated = []() -> size_t
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
		};
		OI::Http::Client stLargeHttp("127.0.0.1", iHttpPort, 30000);
		const size_t iBefore = fnAllocated();
		for (int iIndex = 0; iIndex < 32; ++iIndex)
		{
			auto stResult = co_await stLargeHttp.Get("/large", {});
			if (stResult.bFail || stResult.sBody != std::string(1024 * 1024, 'x'))
			{
				++iBadLocal;
			}
		}
		const size_t iAfter = fnAllocated();
		if (iAfter > iBefore + 8 * 1024 * 1024)
		{
			++iBadLocal;
		}
		printf("hold http release before=%zu after=%zu bad=%d\n", iBefore, iAfter, iBadLocal);
	}

	// 发送预算必须跨会话/通道缓冲连续计数，真正写出才恢复额度。
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
	if (!stBuffered.Send(stPayload) || stBuffered.Send(stPayload))
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
	if (!stBuffered.IsClosed() || stBuffered.Send(stPayload))
	{
		++iBadLocal;
	}
	printf("hold send budget bad=%d\n", iBadLocal);

	// 持续就绪的协程不能饿死同优先级普通任务。
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

	// 子任务异常跨挂起点传回父任务；作用域关闭后不再接受任务。
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
	// 真正的带捕获协程 lambda：临时闭包离开后、挂起恢复后仍访问捕获。
	struct Captures
	{
		std::atomic<int> iDone{0};
		std::atomic<int> iBad{0};
	};
	auto pCaptures = std::make_shared<Captures>();
	for (int iIndexLocal = 0; iIndexLocal < 32; ++iIndexLocal)
	{
		coro::StartTask([pCaptures, text = std::string(4096, 'a' + iIndexLocal % 26), iIndexLocal](coro::Token iTokenParam) -> coro::Task
		{
			co_await coro::yield(::OI::Timer::Milliseconds(0));
			co_await coro::yield(::OI::Timer::Milliseconds(10));
			if (!iTokenParam || text != std::string(4096, 'a' + iIndexLocal % 26))
			{
				pCaptures->iBad.fetch_add(1);
			}
			pCaptures->iDone.fetch_add(1, std::memory_order_release);
		});
	}
	while (pCaptures->iDone.load(std::memory_order_acquire) < 32)
	{
		co_await coro::yield(::OI::Timer::Milliseconds(10));
	}
	iBadLocal += pCaptures->iBad.load();
	printf("hold captured tasks done=%d bad=%d\n", pCaptures->iDone.load(), pCaptures->iBad.load());

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
	} stPending;
	{
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
	}
	printf("hold preconnect send budget bad=%d\n", iBadLocal);
	// 标准回调的可选上下文跨挂起保持有效。
	{
		using Req = OI::RpcData::HoldReq;
		using Res = OI::RpcData::HoldRes;
		auto fnBody = []()
		{
			OI::Base::BytesStream stBytes;
			stBytes << (uint64_t(1) | uint64_t(RPC_REQ_MASK));
			Req stRequestLocal;
			stRequestLocal.iMark = 73;
			stRequestLocal.serialize(stBytes);
			return stBytes;
		};
		int iCallbacks = 0;
		auto fnContextHandler = [&](const OI::Rpc::RpcContext& stContextParam, const Req& stRequestParam, Res&) -> OI::Rpc::RpcTask
		{
			co_await coro::yield(0);
			if (stContextParam.iToken != 88 || &stContextParam.stManager != &stPending || stContextParam.iSessionId != 77 || stRequestParam.iMark != 73)
			{
				++iBadLocal;
			}
			++iCallbacks;
			co_return 1001;
		};
		co_await OI::Rpc::RunHandledRpc<Req, Res>(fnContextHandler, OI::Rpc::RpcProtoId<Res>::iValue, 88, &stPending, 77, fnBody());
		if (iCallbacks != 1)
		{
			++iBadLocal;
		}
	}

	stPending.SetPendingCallLimits(1, 8);
	OI::Base::BytesStream stFrame;
	stFrame << uint64_t(1);
	int iFirstFirst = 0, iSecondSecond = 0;
	auto fnBind = [&](int& iResultParam)
	{
		return OI::Rpc::BindRpcCall(&stPending, INVALID_SID, 0, {}, &iResultParam, 0, {}, []()
		{
		});
	};
	auto iFirstA = fnBind(iFirstFirst);
	stPending.TryPendCall(iFirstA, stFrame.GetMutableBuff());
	auto iSecondB = fnBind(iSecondSecond);
	stPending.TryPendCall(iSecondB, stFrame.GetMutableBuff());
	if (iSecondSecond != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	OI::Rpc::WakeRpcCall(iFirstA, OI_ERR_TIMEOUT, nullptr);
	iSecondSecond = 0;
	iSecondB = fnBind(iSecondSecond);
	stPending.TryPendCall(iSecondB, stFrame.GetMutableBuff());
	if (iSecondSecond != 0)
	{
		++iBadLocal;
	}
	stPending.FailHeldCalls();
	if (iSecondSecond != OI_ERR_CONNECT)
	{
		++iBadLocal;
	}
	stPending.SetPendingCallLimits(8, 7);
	iSecondSecond = 0;
	iSecondB = fnBind(iSecondSecond);
	stPending.TryPendCall(iSecondB, stFrame.GetMutableBuff());
	if (iSecondSecond != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	printf("hold pending limits bad=%d\n", iBadLocal);

	// 登记后序列化异常，以及发送异常与完成回调竞争；恢复后不能留下悬空等待项。
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
	auto fnBalanced = [stBaseline]()
	{
		const auto stNow = OI::Rpc::GetRpcWaitStats();
		return stNow.iCalls == stBaseline.iCalls && stNow.iExpirations == stBaseline.iExpirations && stNow.iBuckets == stBaseline.iBuckets && stNow.iSessionEntries == stBaseline.iSessionEntries && stNow.iSessionBuckets == stBaseline.iSessionBuckets;
	};
	// 会话索引换绑后，旧连接断开只能完成仍属于它的调用。
	std::vector<int32_t> vIndexedResults(4096, -99);
	std::vector<uint64_t> vIndexedCalls;
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		const auto iCallLocal = OI::Rpc::BindRpcCall(&stPending, iIndexLocal < 32 ? 123 : 456, 0, {}, &vIndexedResults[iIndexLocal], 180000, {}, []()
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
	OI::Rpc::FailRpcCalls(&stPending, 123);
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		if (vIndexedResults[iIndexLocal] != (iIndexLocal >= 16 && iIndexLocal < 32 ? OI_ERR_CONNECT : -99))
		{
			++iBadLocal;
		}
	}
	OI::Rpc::FailRpcCalls(&stPending, 789);
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		if (vIndexedResults[iIndexLocal] != (iIndexLocal < 32 ? OI_ERR_CONNECT : -99))
		{
			++iBadLocal;
		}
		OI::Rpc::WakeRpcCall(vIndexedCalls[iIndexLocal], OI_ERR_CONNECT, nullptr);
	}
	if (!fnBalanced())
	{
		++iBadLocal;
	}
	printf("hold rpc session index calls=4096 bad=%d\n", iBadLocal);
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
		co_await OI::Rpc::RpcCall(stPending, INVALID_SID, stThrowingBase, stUnused, OI::Timer::Milliseconds(50));
	}
	catch (const std::runtime_error&)
	{
		++iCaughtErrors;
	}
	if (!fnBalanced())
	{
		++iBadLocal;
	}
	stPending.SetPendingCallLimits(64, 64 * 1024 * 1024);
	OI::RpcData::HoldReq stOrdinary;
	for (int iMode = 1; iMode <= 4; ++iMode)
	{
		stPending.iFailureMode = iMode;
		for (int iIndexLocal = 0; iIndexLocal < 128; ++iIndexLocal)
		{
			try
			{
				co_await OI::Rpc::RpcCall(stPending, stOrdinary, stUnused, OI::Timer::Milliseconds(50));
			}
			catch (const std::runtime_error&)
			{
				++iCaughtErrors;
			}
			if (!fnBalanced())
			{
				++iBadLocal;
			}
		}
	}
	stPending.iFailureMode = 0;
	co_await coro::yield(OI::Timer::Milliseconds(250));
	if (iCaughtErrors != 513 || !fnBalanced())
	{
		++iBadLocal;
	}
	printf("hold rpc exceptions caught=%d bad=%d\n", iCaughtErrors, iBadLocal);

	// 每个分片留一条长超时请求堵住队首，再拒绝十万条；索引数量必须保持不变。
	stPending.SetPendingCallLimits(64, 64 * 8);
	int vAnchorResults[64]{};
	for (int iIndexLocal = 0; iIndexLocal < 64; ++iIndexLocal)
	{
		auto iIdLocal = OI::Rpc::BindRpcCall(&stPending, INVALID_SID, 0, {}, &vAnchorResults[iIndexLocal], 180000, {}, []()
		{
		});
		stPending.TryPendCall(iIdLocal, stFrame.GetMutableBuff());
	}
	const auto stAnchored = OI::Rpc::GetRpcWaitStats();
	if (stAnchored.iCalls != stBaseline.iCalls + 64 || stAnchored.iExpirations != stBaseline.iExpirations + 64)
	{
		++iBadLocal;
	}
	for (int iIndexLocal = 0; iIndexLocal < 100000; ++iIndexLocal)
	{
		int iRejected = 0;
		auto iIdLocal = OI::Rpc::BindRpcCall(&stPending, INVALID_SID, 0, {}, &iRejected, 180000 + iIndexLocal % 17, {}, []()
		{
		});
		stPending.TryPendCall(iIdLocal, stFrame.GetMutableBuff());
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
	stPending.FailHeldCalls();
	for (int iCodeLocal : vAnchorResults)
	{
		if (iCodeLocal != OI_ERR_CONNECT)
		{
			++iBadLocal;
		}
	}
	if (!fnBalanced())
	{
		++iBadLocal;
	}
	printf("hold rpc expiry rejected=100000 retained=%zu bad=%d\n", stLimited.iExpirations - stBaseline.iExpirations, iBadLocal);

	// 只在内部配对入口注入错误来源，不构造另一套网络客户端。
	auto& stManagerLocal = IpcClient::GetInstance();
	const auto iResponseIdLocal = ::OI::Rpc::RpcProtoId<::OI::RpcData::HoldRes>::iValue;
	int iDecoded = 0, iResumed = 0, iResultLocal = -99;
	uint64_t iCall = ::OI::Rpc::BindRpcCall(&stManagerLocal, m_iSessionId, iResponseIdLocal, {}, &iResultLocal, 0, [&](::OI::Base::BytesStream&)
	{
		++iDecoded;
		return true;
	}, [&]()
	{
		++iResumed;
	});
	auto fnDeliver = [&](::OI::Net::SessionManager* pSourceParam, sid_t iSessionIdParam)
	{
		::OI::Base::BytesStream stBody;
		stBody << iCall;
		::OI::Rpc::DeliverRpc(iResponseIdLocal, stBody, pSourceParam, iSessionIdParam);
	};
	fnDeliver(&stManagerLocal, m_iSessionId + 100000);
	fnDeliver(&::OI::Redis::RedisClient::GetInstance(), m_iSessionId);
	if (iDecoded || iResumed)
	{
		++iBadLocal;
	}
	fnDeliver(&stManagerLocal, m_iSessionId);
	if (iDecoded != 1 || iResumed != 1)
	{
		++iBadLocal;
	}
	iResumed = 0;
	const sid_t iFakeSessionId = m_iSessionId + 100000;
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
		auto stBroken = ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, stResponseLocal);
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
	stRequestLocal.iDelayMilliseconds = 10000;
	stRequestLocal.iMark = 2;
	stResponseLocal.iMark = 0;
	auto stTimeout = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(2));
	iReturnCodeLocal = stTimeout.GetRet();
	printf("hold rpc timeout ret=%d\n", iReturnCodeLocal);
	fflush(stdout);
	if (iReturnCodeLocal != OI_ERR_TIMEOUT)
	{
		iBadLocal += 1;
	}
	// 回包还在路上。让这一下把迟到的包和已经武装的超时定时器都消化掉。
	co_await coro::yield(10000);
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

	const int vDelays[] = {9800, 9900, 10000, 10100, 10200, 10400, 10600, 10800};
	const int iNumber = 8;
	// 每个调用自己持有一份。没写完之前，这里退出也不会把盒子释放掉。
	std::shared_ptr<RaceBox> pBox(new RaceBox());
	iCpuBegin = std::clock();
	iWallBegin = ::OI::Timer::SteadyMilliseconds();
	for (int iIndexLocal = 0; iIndexLocal < iNumber; ++iIndexLocal)
	{
		int iMark = 100 + iIndexLocal;
		int iDelay = vDelays[iIndexLocal];
		coro::StartTask([iMark, iDelay, iIndexLocal, pBox](coro::Token)
		{
			return RaceOne(iIndexLocal, iMark, iDelay, pBox);
		});
	}
	// 单次超时是 10 秒。等都写完再读，避免读到写了一半的结果。
	int iWaited = 0;
	const int iItemIndexCapacityMilliseconds = 20000;
	while (pBox->iFinished.load(std::memory_order_acquire) < iNumber && iWaited < iItemIndexCapacityMilliseconds)
	{
		co_await coro::yield(50);
		iWaited += 50;
	}
	int iDoneLocal = pBox->iFinished.load(std::memory_order_acquire);
	int iReplyNumber = 0;
	int iTimeoutNumber = 0;
	int iRaceBad = iDoneLocal == iNumber ? 0 : 1;
	if (iDoneLocal == iNumber)
	{
		for (int iIndexLocal = 0; iIndexLocal < iNumber; ++iIndexLocal)
		{
			if (pBox->vReturnCodes[iIndexLocal] == 0 && pBox->vMarks[iIndexLocal] == 100 + iIndexLocal)
			{
				iReplyNumber += 1;
			}
			else if (pBox->vReturnCodes[iIndexLocal] == OI_ERR_TIMEOUT)
			{
				iTimeoutNumber += 1;
			}
			else
			{
				iRaceBad += 1;
			}
		}
	}
	iBadLocal += iRaceBad;
	printf("hold rpc race n=%d done=%d reply=%d timeout=%d bad=%d cpu_ms=%lld wall_ms=%lld\n",
	    iNumber, iDoneLocal, iReplyNumber, iTimeoutNumber, iRaceBad, CpuMs(iCpuBegin), WallMs(iWallBegin));
	fflush(stdout);

	int iSpins = 0;
	int iLimit = m_iRedisTimeoutMilliseconds > 0 ? m_iRedisTimeoutMilliseconds / 50 : 100;
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
			iBadLocal += 1;
			printf("hold done bad=%d\n", iBadLocal);
			fflush(stdout);
			TestBoard::GetInstance().Done(1);
			co_return;
		}
		co_await coro::yield(50);
	}

	::OI::Redis::RedisResult stPing;
	{
		::OI::Redis::RedisCall stCall = ::OI::Redis::CallRedis({"PING"}, stPing);
		co_await stCall;
	}
	printf("hold redis ping ok=%d\n", Pong(stPing) ? 1 : 0);
	fflush(stdout);
	if (!Pong(stPing))
	{
		iBadLocal += 1;
	}

	int iSeconds = m_iRedisTimeoutMilliseconds / 1000;
	if (iSeconds < 1)
	{
		iSeconds = 1;
	}
	iCpuBegin = std::clock();
	iWallBegin = ::OI::Timer::SteadyMilliseconds();
	::OI::Redis::RedisResult stLate;
	{
		stLate = ::OI::Redis::CallRedisSync({"BLPOP", "hold-late", std::to_string(iSeconds + 5)});
	}
	printf("hold redis late timeout=%d cpu_ms=%lld wall_ms=%lld\n",
	    stLate.bTimeout ? 1 : 0, CpuMs(iCpuBegin), WallMs(iWallBegin));
	fflush(stdout);
	if (!stLate.bTimeout)
	{
		iBadLocal += 1;
	}
	::OI::Redis::RedisResult stPingAfterLate;
	{
		::OI::Redis::RedisCall stCall = ::OI::Redis::CallRedis({"PING"}, stPingAfterLate);
		co_await stCall;
	}
	printf("hold redis late ping ok=%d\n", Pong(stPingAfterLate) ? 1 : 0);
	fflush(stdout);
	if (!Pong(stPingAfterLate))
	{
		iBadLocal += 1;
	}

	iCpuBegin = std::clock();
	iWallBegin = ::OI::Timer::SteadyMilliseconds();
	::OI::Redis::RedisResult stRace;
	{
		::OI::Redis::RedisCall stCall = ::OI::Redis::CallRedis({"BLPOP", "hold-race", std::to_string(iSeconds)}, stRace);
		co_await stCall;
	}
	int iNil = stRace.pReply && stRace.pReply->type == REDIS_REPLY_NIL ? 1 : 0;
	printf("hold redis race timeout=%d nil=%d cpu_ms=%lld wall_ms=%lld\n",
	    stRace.bTimeout ? 1 : 0, iNil, CpuMs(iCpuBegin), WallMs(iWallBegin));
	fflush(stdout);
	if (!stRace.bTimeout && !iNil)
	{
		iBadLocal += 1;
	}
	::OI::Redis::RedisResult stPingAfterRace;
	{
		::OI::Redis::RedisCall stCall = ::OI::Redis::CallRedis({"PING"}, stPingAfterRace);
		co_await stCall;
	}
	printf("hold redis race ping ok=%d\n", Pong(stPingAfterRace) ? 1 : 0);
	fflush(stdout);
	if (!Pong(stPingAfterRace))
	{
		iBadLocal += 1;
	}

	// 真断开当前会话：已发出的 RPC 应及时失败，不能继续等十秒超时。
	stRequestLocal.iDelayMilliseconds = 5000;
	sid_t iCloseSessionId = m_iSessionId;
	coro::StartTask([iCloseSessionId](coro::Token) -> coro::Task
	{
		co_await coro::yield(::OI::Timer::Milliseconds(100));
		IpcClient::GetInstance().CloseSession(iCloseSessionId);
	});
	iWallBegin = ::OI::Timer::SteadyMilliseconds();
	auto stClosed = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::HoldRes>(stRequestLocal, ::OI::Timer::Seconds(10));
	if (stClosed.GetRet() != OI_ERR_CONNECT || WallMs(iWallBegin) > 2000)
	{
		++iBadLocal;
	}
	printf("hold rpc disconnected code=%d wall_ms=%lld\n", stClosed.GetRet(), WallMs(iWallBegin));
	printf("hold done bad=%d\n", iBadLocal);
	fflush(stdout);
	TestBoard::GetInstance().Done(iBadLocal == 0 ? 0 : 1);
	co_return;
}
