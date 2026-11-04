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
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <string>

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

static long long WallMs(std::chrono::steady_clock::time_point stBeginParam)
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - stBeginParam)
	    .count();
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

static coro::Task RaceOne(sid_t iSessionIdParam, int32_t iIndexParam, int32_t iMarkParam, int32_t iDelayParam, std::shared_ptr<RaceBox> pBoxParam)
{
	::OI::RpcData::HoldReq stOne;
	::OI::RpcData::HoldRes stBack;
	stOne.iDelayMilliseconds = iDelayParam;
	stOne.iMark = iMarkParam;
	int iCodeLocal = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), iSessionIdParam, stOne, stBack, 10);
	pBoxParam->vReturnCodes[iIndexParam] = iCodeLocal;
	pBoxParam->vMarks[iIndexParam] = stBack.iMark;
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
	const auto stDeadline = std::chrono::steady_clock::now() + OI::Timer::Seconds(3);
	while ((stEager.Connections() != 1 || stDelayedAwait.Connections() != 1) && std::chrono::steady_clock::now() < stDeadline)
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
	const auto stDeadline = std::chrono::steady_clock::now() + OI::Timer::Seconds(5);
	while (!stRedis.IsConnect() && std::chrono::steady_clock::now() < stDeadline)
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
			auto stResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, OI::Timer::Seconds(3));
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

coro::Task HoldMgr::Pump()
{
	int iBadLocal = co_await CheckTimerExceptions();
	if (iBadLocal != 0)
	{
		printf("hold done bad=%d\n", iBadLocal);
		fflush(stdout);
		TestBoard::GetInstance().Done(1);
		co_return;
	}
	iBadLocal += co_await CheckNestedResults();
	iBadLocal += co_await CheckDeferredRpcSession();
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
		const auto stDeadline = std::chrono::steady_clock::now() + OI::Timer::Seconds(3);
		while (!pWeak.expired() && std::chrono::steady_clock::now() < stDeadline)
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
	const auto stTaskDeadline = std::chrono::steady_clock::now() + OI::Timer::Seconds(3);
	while (!pOrdinary->load() && std::chrono::steady_clock::now() < stTaskDeadline)
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
		iResult = co_await OI::Rpc::RpcCall(IpcClient::GetInstance(), stRequest, stResponse, OI::Timer::Seconds(2));
		if (iResult != 0 || stResponse.iMark != 92)
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
	const auto stFairnessBegin = std::chrono::steady_clock::now();
	for (int iIndexLocal = 0; iIndexLocal < 64; ++iIndexLocal)
	{
		stBusyScope.Start([&bOrdinaryDone, stFairnessBegin](coro::Token, coro::StopToken stStopParam) -> coro::Task
		{
			while (!bOrdinaryDone.load() && !stStopParam.StopRequested() && std::chrono::steady_clock::now() - stFairnessBegin < OI::Timer::Seconds(3))
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
	if (WallMs(stFairnessBegin) > 1500)
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
		bool TryPendCall(uint64_t iIdParam, const OI::Base::Bytes& stDataParam) override
		{
			if (!iFailureMode)
			{
				return ProtoClientManager::TryPendCall(iIdParam, stDataParam);
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
	// 可选上下文跨挂起保持有效，旧五参数无返回值处理函数仍可注册。
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
		auto fnLegacyHandler = [&](coro::Token iTokenParam, OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, Req&, Res&) -> coro::Task
		{
			if (iTokenParam != 88 || pManagerParam != &stPending || iSessionIdParam != 77)
			{
				++iBadLocal;
			}
			++iCallbacks;
			co_return;
		};
		co_await OI::Rpc::RunHandledRpc<Req, Res>(fnLegacyHandler, OI::Rpc::RpcProtoId<Res>::iValue, 88, &stPending, 77, fnBody());
		if (iCallbacks != 2)
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
	auto fnBalanced = [&]()
	{
		const auto stNow = OI::Rpc::GetRpcWaitStats();
		return stNow.iCalls == stBaseline.iCalls && stNow.iExpirations == stBaseline.iExpirations && stNow.iBuckets == stBaseline.iBuckets;
	};
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
				co_await OI::Rpc::RpcCall(stPending, INVALID_SID, stOrdinary, stUnused, OI::Timer::Milliseconds(50));
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
	auto stDirect = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, ::OI::Timer::Seconds(10));
	if (!stDirect || stDirect.GetRes().iMark != 1)
	{
		++iBadLocal;
	}
	static_assert(std::is_same_v<decltype(stDirect.GetRet()), int32_t>);
	stRequestLocal.iDelayMilliseconds = -1;
	auto stFailed = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal);
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
	auto stShortWait = co_await ::OI::Rpc::RpcCall<::OI::RpcData::HoldRes>(stManagerLocal, m_iSessionId, stRequestLocal, ::OI::Timer::Milliseconds(150));
	if (stShortWait.GetRet() != OI_ERR_TIMEOUT)
	{
		++iBadLocal;
	}
	co_await coro::yield(::OI::Timer::Milliseconds(700));
	stRequestLocal.iDelayMilliseconds = 0;
	int iReturnCodeLocal = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), m_iSessionId, stRequestLocal, stResponseLocal, 10);
	printf("hold rpc early ret=%d mark=%d\n", iReturnCodeLocal, stResponseLocal.iMark);
	fflush(stdout);
	if (iReturnCodeLocal != 0 || stResponseLocal.iMark != 1)
	{
		iBadLocal += 1;
	}

	std::clock_t iCpuBegin = std::clock();
	auto stWallBegin = std::chrono::steady_clock::now();
	stRequestLocal.iDelayMilliseconds = 10000;
	stRequestLocal.iMark = 2;
	stResponseLocal.iMark = 0;
	iReturnCodeLocal = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), m_iSessionId, stRequestLocal, stResponseLocal, 2);
	printf("hold rpc timeout ret=%d\n", iReturnCodeLocal);
	fflush(stdout);
	if (iReturnCodeLocal != OI_ERR_TIMEOUT)
	{
		iBadLocal += 1;
	}
	// 回包还在路上。让这一下把迟到的包和已经武装的超时定时器都消化掉。
	co_await coro::yield(10000);
	printf("hold rpc late-reply cpu_ms=%lld wall_ms=%lld\n",
	    CpuMs(iCpuBegin), WallMs(stWallBegin));
	fflush(stdout);

	stRequestLocal.iDelayMilliseconds = 0;
	stRequestLocal.iMark = 3;
	stResponseLocal.iMark = 0;
	iReturnCodeLocal = co_await ::OI::Rpc::RpcCall(IpcClient::GetInstance(), m_iSessionId, stRequestLocal, stResponseLocal, 10);
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
	sid_t iSessionIdLocal = m_iSessionId;
	iCpuBegin = std::clock();
	stWallBegin = std::chrono::steady_clock::now();
	for (int iIndexLocal = 0; iIndexLocal < iNumber; ++iIndexLocal)
	{
		int iMark = 100 + iIndexLocal;
		int iDelay = vDelays[iIndexLocal];
		coro::StartTask([iMark, iDelay, iIndexLocal, pBox, iSessionIdLocal](coro::Token)
		{
			return RaceOne(iSessionIdLocal, iIndexLocal, iMark, iDelay, pBox);
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
	    iNumber, iDoneLocal, iReplyNumber, iTimeoutNumber, iRaceBad, CpuMs(iCpuBegin), WallMs(stWallBegin));
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
	stWallBegin = std::chrono::steady_clock::now();
	::OI::Redis::RedisResult stLate;
	{
		stLate = ::OI::Redis::CallRedisSync({"BLPOP", "hold-late", std::to_string(iSeconds + 5)});
	}
	printf("hold redis late timeout=%d cpu_ms=%lld wall_ms=%lld\n",
	    stLate.bTimeout ? 1 : 0, CpuMs(iCpuBegin), WallMs(stWallBegin));
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
	stWallBegin = std::chrono::steady_clock::now();
	::OI::Redis::RedisResult stRace;
	{
		::OI::Redis::RedisCall stCall = ::OI::Redis::CallRedis({"BLPOP", "hold-race", std::to_string(iSeconds)}, stRace);
		co_await stCall;
	}
	int iNil = stRace.pReply && stRace.pReply->type == REDIS_REPLY_NIL ? 1 : 0;
	printf("hold redis race timeout=%d nil=%d cpu_ms=%lld wall_ms=%lld\n",
	    stRace.bTimeout ? 1 : 0, iNil, CpuMs(iCpuBegin), WallMs(stWallBegin));
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
	stWallBegin = std::chrono::steady_clock::now();
	auto stClosed = co_await ::OI::Rpc::RpcCall<::OI::RpcData::HoldRes>(stManagerLocal, m_iSessionId, stRequestLocal, ::OI::Timer::Seconds(10));
	if (stClosed.GetRet() != OI_ERR_CONNECT || WallMs(stWallBegin) > 2000)
	{
		++iBadLocal;
	}
	printf("hold rpc disconnected code=%d wall_ms=%lld\n", stClosed.GetRet(), WallMs(stWallBegin));
	printf("hold done bad=%d\n", iBadLocal);
	fflush(stdout);
	TestBoard::GetInstance().Done(iBadLocal == 0 ? 0 : 1);
	co_return;
}
