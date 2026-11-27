#include "test_rpc_pending.h"
#include "hold_test.h"
#include "oi_rpc_session_manager.h"
#include "oi_rpc_peer.h"
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
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace
{
std::atomic<bool> bBatchBindFailure{false};
struct BatchRequest : OI::RpcData::HoldReq
{
};
struct BatchResponse : OI::RpcData::HoldRes
{
};
struct BatchResponseId
{
	constexpr operator proto_type_t() const
	{
		if (!std::is_constant_evaluated() && bBatchBindFailure.exchange(false))
		{
			throw std::bad_alloc();
		}
		return OI::Rpc::RpcProtoId<OI::RpcData::HoldRes>::iValue;
	}
};
}
namespace OI::Rpc
{
template <>
struct RpcProtoId<BatchRequest>
{
	using Response = BatchResponse;
	static constexpr proto_type_t iValue = RpcProtoId<OI::RpcData::HoldReq>::iValue;
};
template <>
struct RpcProtoId<BatchResponse>
{
	static constexpr BatchResponseId iValue{};
};
}

// 使用真实待连接队列验证条数、字节限制以及取消后的容量恢复。
struct TestRpcPending::PendingClient : OI::Rpc::ProtoClientManager
{
	int iFailureMode = 0;
	std::atomic<sid_t> iProbeSession{INVALID_SID};
	std::atomic<int32_t> iProbeDisconnects{0};
	void SetSendLimit(size_t iBytes)
	{
		iMaxSendQueueBytes = iBytes;
	}
	OI::Rpc::ProtoSession* AttachForTest()
	{
		auto pSessionLocal = new OI::Rpc::ProtoSession(this);
		AddSession(pSessionLocal);
		return pSessionLocal;
	}
	void OnClientConnect(OI::Net::Session* pSession) override
	{
		iProbeSession.store(pSession->GetSessionID());
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
		iProbeDisconnects.fetch_add(1);
	}
	void SubmitCall(uint64_t iId, const OI::Base::Bytes& stData) override
	{
		if (!iFailureMode)
		{
			ProtoClientManager::SubmitCall(iId, stData);
			return;
		}
		if (iFailureMode == 2)
		{
			OI::Rpc::RpcCallManager::GetInstance().Wake(iId, OI_ERR_FAIL, nullptr);
		}
		if (iFailureMode == 3)
		{
			ProtoClientManager::TryPendCall(iId, stData);
		}
		if (iFailureMode == 4)
		{
			OI::Coro::StartTask([iId]() -> OI::Coro::Task
			{
				OI::Rpc::RpcCallManager::GetInstance().Wake(iId, OI_ERR_FAIL, nullptr);
				co_return;
			});
		}
		throw std::runtime_error("injected submission failure");
	}
};

bool TestRpcPending::SameWaitStats(const OI::Rpc::RpcWaitStats& stBaseline)
{
	const auto stNow = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	// clang-format off
	return stNow.iCalls == stBaseline.iCalls
		&& stNow.iExpirations == stBaseline.iExpirations
		&& stNow.iBuckets == stBaseline.iBuckets
		&& stNow.iSessionEntries == stBaseline.iSessionEntries
		&& stNow.iSessionBuckets == stBaseline.iSessionBuckets;
	// clang-format on
}

int TestRpcPending::CheckPreconnectSendBudget()
{
	int iBadLocal = 0;
	OI::Base::Bytes stPayload("12345678", 8);
	PendingClient stRawPending;
	stRawPending.SetSendLimit(8);
	if (stRawPending.SendRawOrPend(stPayload, 1024) != OI_OK || stRawPending.SendRawOrPend(stPayload, 1024) != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	auto pSessionLocal = stRawPending.AttachForTest();
	if (pSessionLocal->PendingSendBytes() != 8 || stRawPending.SendRaw(stPayload) != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	stRawPending.CloseSession(pSessionLocal->GetSessionID());
	pSessionLocal->Release();
	LOG_TRACE("hold preconnect send budget bad=%d", iBadLocal);
	return iBadLocal;
}

// 标准回调的可选上下文跨挂起保持有效。
OI::Coro::TaskResult<int> TestRpcPending::CheckRpcContext(PendingClient& stPending)
{
	using Req = OI::RpcData::HoldReq;
	using Res = OI::RpcData::HoldRes;
	int iBadLocal = 0;
	Req stRequestLocal;
	stRequestLocal.iMark = 73;
	int iCallbacks = 0;
	auto fnContextHandler = [&](const OI::Rpc::RpcContext& stContext, const Req& stRequest, Res&) -> OI::Rpc::RpcTask
	{
		co_await OI::Coro::yield(0);
		if (stContext.iToken != 88 || &stContext.stManager != &stPending || stContext.iSessionId != 77 || stRequest.iMark != 73)
		{
			++iBadLocal;
		}
		++iCallbacks;
		co_return 1001;
	};
	co_await OI::Rpc::RpcHandlerBuilder::Run<Req, Res>(fnContextHandler, OI::Rpc::RpcProtoId<Res>::iValue, 88, &stPending, 77,
	    uint64_t(1) | uint64_t(RPC_REQ_MASK), std::move(stRequestLocal));
	if (iCallbacks != 1)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold rpc context bad=%d", iBadLocal);
	co_return iBadLocal;
}

uint64_t TestRpcPending::BindPendingCall(PendingClient& stPending, int32_t* pResult, int64_t iTimeoutMilliseconds)
{
	return OI::Rpc::RpcCallManager::GetInstance().Bind(&stPending, INVALID_SID, 0, {}, pResult, iTimeoutMilliseconds, {}, []()
	{
	});
}

int TestRpcPending::CheckPendingLimits(PendingClient& stPending)
{
	int iBadLocal = 0;
	stPending.SetPendingCallLimits(1, 8);
	OI::Base::BytesStream stFrame;
	stFrame << uint64_t(1);
	int32_t iFirstResult = 0, iSecondResult = 0;
	const auto iFirstCall = BindPendingCall(stPending, &iFirstResult, 0);
	stPending.TryPendCall(iFirstCall, stFrame.GetMutableBuff());
	auto iSecondCall = BindPendingCall(stPending, &iSecondResult, 0);
	stPending.TryPendCall(iSecondCall, stFrame.GetMutableBuff());
	if (iSecondResult != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	OI::Rpc::RpcCallManager::GetInstance().Wake(iFirstCall, OI_ERR_TIMEOUT, nullptr);
	iSecondResult = 0;
	iSecondCall = BindPendingCall(stPending, &iSecondResult, 0);
	stPending.TryPendCall(iSecondCall, stFrame.GetMutableBuff());
	if (iSecondResult != 0)
	{
		++iBadLocal;
	}
	stPending.FailHeldCalls();
	if (iSecondResult != OI_ERR_CONNECT)
	{
		++iBadLocal;
	}
	stPending.SetPendingCallLimits(8, 7);
	iSecondResult = 0;
	iSecondCall = BindPendingCall(stPending, &iSecondResult, 0);
	stPending.TryPendCall(iSecondCall, stFrame.GetMutableBuff());
	if (iSecondResult != OI_ERR_BUSY)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold pending limits bad=%d", iBadLocal);
	return iBadLocal;
}

// 会话索引换绑后，旧连接断开只能完成仍属于它的调用。
int TestRpcPending::CheckRpcSessionIndex(PendingClient& stPending)
{
	int iBadLocal = 0;
	const auto stBaseline = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	std::vector<int32_t> vIndexedResults(4096, -99);
	std::vector<uint64_t> vIndexedCalls;
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		const auto iCallLocal = OI::Rpc::RpcCallManager::GetInstance().Bind(&stPending, iIndexLocal < 32 ? 123 : 456, 0, {}, &vIndexedResults[iIndexLocal], 180000, {}, []()
		{
		});
		vIndexedCalls.push_back(iCallLocal);
	}
	for (size_t iIndexLocal = 0; iIndexLocal < 16; ++iIndexLocal)
	{
		if (!OI::Rpc::RpcCallManager::GetInstance().ClaimSend(vIndexedCalls[iIndexLocal], 789))
		{
			++iBadLocal;
		}
	}
	OI::Rpc::RpcCallManager::GetInstance().FailSession(&stPending, 123);
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		if (vIndexedResults[iIndexLocal] != (iIndexLocal >= 16 && iIndexLocal < 32 ? OI_ERR_CONNECT : -99))
		{
			++iBadLocal;
		}
	}
	OI::Rpc::RpcCallManager::GetInstance().FailSession(&stPending, 789);
	for (size_t iIndexLocal = 0; iIndexLocal < vIndexedResults.size(); ++iIndexLocal)
	{
		if (vIndexedResults[iIndexLocal] != (iIndexLocal < 32 ? OI_ERR_CONNECT : -99))
		{
			++iBadLocal;
		}
		OI::Rpc::RpcCallManager::GetInstance().Wake(vIndexedCalls[iIndexLocal], OI_ERR_CONNECT, nullptr);
	}
	if (!SameWaitStats(stBaseline))
	{
		++iBadLocal;
	}
	LOG_TRACE("hold rpc session index calls=4096 bad=%d", iBadLocal);
	return iBadLocal;
}

// 跨分片、跨批次断连清理：回调抛异常也必须完成后续项并归还所有 guard。
int TestRpcPending::CheckRpcDisconnectDrain(PendingClient& stPending)
{
	const auto stBaseline = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	std::vector<int32_t> vResults(4096, -99);
	auto pGuard = std::make_shared<std::atomic<int32_t>>(0);
	auto pCallbacks = std::make_shared<std::atomic<int32_t>>(0);
	for (size_t iIndex = 0; iIndex < vResults.size(); ++iIndex)
	{
		OI::Rpc::RpcCallManager::GetInstance().Bind(&stPending, 987654, 0, {}, &vResults[iIndex], 180000, {}, [pCallbacks]()
		{
			if (pCallbacks->fetch_add(1) == 0)
			{
				throw std::bad_alloc();
			}
		}, pGuard);
	}
	OI::Rpc::RpcCallManager::GetInstance().FailSession(&stPending, 987654);
	int iBad = pCallbacks->load() == 4096 && pGuard->load() == 0 && SameWaitStats(stBaseline) ? 0 : 1;
	for (const auto& iResult : vResults)
	{
		if (iResult != OI_ERR_CONNECT)
		{
			++iBad;
		}
	}
	// 建连失败时同批唤醒：首个回调抛异常，后续已摘出的请求仍要完成。
	auto pPeer = OI::Rpc::PeerConnection::Create(&stPending, 1, "127.0.0.1", "1", OI::Rpc::PeerPolicy(&stPending), false);
	std::vector<int32_t> vPeerResults(8, -99);
	auto pPeerCallbacks = std::make_shared<std::atomic<int32_t>>(0);
	auto pPeerGuard = std::make_shared<std::atomic<int32_t>>(0);
	std::vector<uint64_t> vPeerCalls;
	for (auto& iResult : vPeerResults)
	{
		const auto iCall = OI::Rpc::RpcCallManager::GetInstance().Bind(&stPending, 123456, 0, {}, &iResult, 0, {}, [pPeerCallbacks]()
		{
			if (pPeerCallbacks->fetch_add(1) == 0)
			{
				throw std::runtime_error("injected peer wake failure");
			}
		}, pPeerGuard);
		vPeerCalls.push_back(iCall);
		pPeer->vHeld.push_back({iCall, {}});
	}
	int iPeerThrew = 0;
	try
	{
		pPeer->Finish(INVALID_SID);
	}
	catch (...)
	{
		++iPeerThrew;
	}
	int iPeerBad = pPeerCallbacks->load() == 8 && pPeerGuard->load() == 0 && iPeerThrew == 0 && SameWaitStats(stBaseline) ? 0 : 1;
	for (const auto iCall : vPeerCalls)
	{
		OI::Rpc::RpcCallManager::GetInstance().Abandon(iCall);
	}
	OI::Rpc::RpcCallManager::WaitGuard(pPeerGuard);
	for (const auto iResult : vPeerResults)
	{
		if (iResult != OI_ERR_CONNECT)
		{
			++iPeerBad;
		}
	}
	iBad += iPeerBad;
	LOG_TRACE("hold peer finish exception callbacks=%d guard=%d bad=%d", pPeerCallbacks->load(), pPeerGuard->load(), iPeerBad);
	LOG_TRACE("hold disconnect drain calls=4096 bad=%d", iBad);
	return iBad;
}

// 登记后序列化异常，以及发送异常与完成回调竞争；恢复后不能留下悬空等待项。
OI::Coro::TaskResult<int> TestRpcPending::CheckRpcExceptions(PendingClient& stPending)
{
	int iBadLocal = 0;
	const auto stBaseline = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
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
		co_await OI::Rpc::MakeRpcCallAwaiter(stPending, INVALID_SID, stThrowingBase, stUnused, OI::Timer::Milliseconds(50));
	}
	catch (const std::runtime_error&)
	{
		++iCaughtErrors;
	}
	if (!SameWaitStats(stBaseline))
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
				co_await OI::Rpc::MakeRpcCallAwaiter(stPending, stOrdinary, stUnused, OI::Timer::Milliseconds(50));
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
	stPending.iFailureMode = 0;
	co_await OI::Coro::yield(OI::Timer::Milliseconds(250));
	if (iCaughtErrors != 513 || !SameWaitStats(stBaseline))
	{
		++iBadLocal;
	}
	LOG_TRACE("hold rpc exceptions caught=%d bad=%d", iCaughtErrors, iBadLocal);
	co_return iBadLocal;
}

// 第二条登记前分配失败：首条等待超时后清理，再传回原异常。
OI::Coro::TaskResult<int> TestRpcPending::CheckBatchBindFailure(PendingClient& stPending)
{
	const auto stBaseline = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	struct BatchProbe
	{
		PendingClient* pClient;
		size_t iBaselineCalls;
		int iSubmits = 0;
		int iDrops = 0;
		bool bRegistered = false;
	} stProbe{&stPending, stBaseline.iCalls};
	OI::Rpc::RpcSender stSender;
	stSender.pManager = &stPending;
	stSender.pContext = &stProbe;
	stSender.fnSubmit = [](void* pContext, uint64_t iCallId, const OI::Base::Bytes& stData)
	{
		auto& stLocal = *static_cast<BatchProbe*>(pContext);
		++stLocal.iSubmits;
		stLocal.bRegistered = OI::Rpc::RpcCallManager::GetInstance().WaitStats().iCalls == stLocal.iBaselineCalls + 1;
		stLocal.pClient->TryPendCall(iCallId, stData);
		bBatchBindFailure.store(true);
	};
	stSender.fnDrop = [](void* pContext, uint64_t iCallId)
	{
		auto& stLocal = *static_cast<BatchProbe*>(pContext);
		++stLocal.iDrops;
		stLocal.pClient->DropPendingCall(iCallId);
		LOG_TRACE("hold batch first pending dropped");
	};
	stPending.SetPendingCallLimits(64, 64 * 1024 * 1024);
	OI::Rpc::ServiceRoute stRoute;
	stRoute.iStatus = OI::Rpc::ServiceRouteStatus::Ok;
	stRoute.vClients = {&stSender, &stSender};
	BatchRequest stRequest;
	const auto iStart = OI::Timer::SteadyMilliseconds();
	bool bCaught = false;
	try
	{
		co_await OI::Rpc::RouteCall::Send<BatchResponse>(std::move(stRoute), stRequest, OI::Rpc::RpcDeadline(OI::Timer::Milliseconds(200)));
	}
	catch (const std::bad_alloc&)
	{
		bCaught = true;
	}
	bBatchBindFailure.store(false);
	const auto iElapsed = OI::Timer::SteadyMilliseconds() - iStart;
	const int iBad = bCaught && stProbe.bRegistered && stProbe.iSubmits == 1 && stProbe.iDrops == 1 && SameWaitStats(stBaseline) && iElapsed < 3000 ? 0 : 1;
	LOG_TRACE("hold batch bind failure caught=%d registered=%d submits=%d drops=%d elapsed=%lld bad=%d", bCaught, stProbe.bRegistered,
	    stProbe.iSubmits, stProbe.iDrops, static_cast<long long>(iElapsed), iBad);
	co_return iBad;
}

// 每个分片留一条长超时请求堵住队首，再拒绝十万条；索引数量必须保持不变。
int TestRpcPending::CheckRpcExpiry(PendingClient& stPending)
{
	int iBadLocal = 0;
	const auto stBaseline = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	OI::Base::BytesStream stFrame;
	stFrame << uint64_t(1);
	stPending.SetPendingCallLimits(64, 64 * 8);
	int32_t vAnchorResults[64]{};
	for (auto& iAnchorResult : vAnchorResults)
	{
		stPending.TryPendCall(BindPendingCall(stPending, &iAnchorResult, 180000), stFrame.GetMutableBuff());
	}
	const auto stAnchored = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	if (stAnchored.iCalls != stBaseline.iCalls + 64 || stAnchored.iExpirations != stBaseline.iExpirations + 64)
	{
		++iBadLocal;
	}
	for (int iIndexLocal = 0; iIndexLocal < 100000; ++iIndexLocal)
	{
		int32_t iRejected = 0;
		stPending.TryPendCall(BindPendingCall(stPending, &iRejected, 180000 + iIndexLocal % 17), stFrame.GetMutableBuff());
		if (iRejected != OI_ERR_BUSY)
		{
			++iBadLocal;
		}
	}
	const auto stLimited = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	if (stLimited.iCalls != stAnchored.iCalls || stLimited.iExpirations != stAnchored.iExpirations || stLimited.iBuckets != stAnchored.iBuckets)
	{
		++iBadLocal;
	}
	stPending.FailHeldCalls();
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
	LOG_TRACE("hold rpc expiry rejected=100000 retained=%zu bad=%d", stLimited.iExpirations - stBaseline.iExpirations, iBadLocal);
	return iBadLocal;
}

OI::Coro::TaskResult<int> TestRpcPending::CheckDeadlineBuckets()
{
	int iBad = 0;
	auto& stManager = IpcClient::GetInstance();
	const auto stBefore = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	std::vector<uint64_t> vCalls;
	const int64_t iTargetMilliseconds = OI::Timer::SteadyMilliseconds() + 60000;
	for (int iIndex = 0; iIndex < 4096; ++iIndex)
	{
		// 不同剩余时长落到同一小段绝对截止时间，而不是产生4096个时长桶。
		const int64_t iRemainingMilliseconds = iTargetMilliseconds + iIndex % 100 - OI::Timer::SteadyMilliseconds();
		const uint64_t iCallId = OI::Rpc::RpcCallManager::GetInstance().Bind(&stManager, INVALID_SID, 0, {}, nullptr, iRemainingMilliseconds, {}, {});
		vCalls.push_back(iCallId);
	}
	const auto stDuring = OI::Rpc::RpcCallManager::GetInstance().WaitStats();
	if (stDuring.iCalls != stBefore.iCalls + 4096 || stDuring.iBuckets > stBefore.iBuckets + 192)
	{
		++iBad;
	}
	const uint64_t iCallId = OI::Rpc::RpcCallManager::GetInstance().Bind(&stManager, INVALID_SID, 0, {}, nullptr, INT64_MAX, {}, {});
	vCalls.push_back(iCallId);
	for (const auto& iCall : vCalls)
	{
		OI::Rpc::RpcCallManager::GetInstance().Abandon(iCall);
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
		auto fnCheckTimeout = [pProbe, iIndex, iDeadlineMilliseconds]()
		{
			if (OI::Timer::SteadyMilliseconds() < iDeadlineMilliseconds || pProbe->vCodes[iIndex] != OI_ERR_TIMEOUT)
			{
				++pProbe->iBad;
			}
			++pProbe->iCompleted;
		};
		const uint64_t iCallId = OI::Rpc::RpcCallManager::GetInstance().Bind(&stManager, INVALID_SID, 0, {}, &pProbe->vCodes[iIndex], iDelayMilliseconds, {},
		    std::move(fnCheckTimeout), pGuard);
		vCalls.push_back(iCallId);
	}
	const int64_t iLimitMilliseconds = OI::Timer::SteadyMilliseconds() + 4000;
	while (pProbe->iCompleted.load() != 32 && OI::Timer::SteadyMilliseconds() < iLimitMilliseconds)
	{
		co_await OI::Coro::yield(20);
	}
	for (const auto& iCall : vCalls)
	{
		OI::Rpc::RpcCallManager::GetInstance().Abandon(iCall);
	}
	OI::Rpc::RpcCallManager::WaitGuard(pGuard);
	if (pProbe->iCompleted.load() != 32 || pProbe->iBad.load() != 0 || !SameWaitStats(stBefore))
	{
		++iBad;
	}
	// 所有到期项先由同一次 Sweep 摘下；首个回调抛出后其余项仍须完成。
	struct SweepProbe
	{
		std::atomic<int32_t> iCallbacks{0};
		std::atomic<int32_t> iFirstBatch{0};
		std::vector<int32_t> vCodes = std::vector<int32_t>(128, -99);
	};
	auto pSweepProbe = std::make_shared<SweepProbe>();
	auto pSweepGuard = std::make_shared<std::atomic<int32_t>>(0);
	vCalls.clear();
	const int64_t iSweepDeadline = OI::Timer::SteadyMilliseconds() + 500;
	for (size_t iIndex = 0; iIndex < pSweepProbe->vCodes.size(); ++iIndex)
	{
		const int64_t iRemaining = iSweepDeadline - OI::Timer::SteadyMilliseconds();
		auto fnCheckSweep = [pSweepProbe, pSweepGuard]()
		{
			if (pSweepProbe->iCallbacks.fetch_add(1) == 0)
			{
				pSweepProbe->iFirstBatch = pSweepGuard->load() >= 2 ? 1 : -1;
				throw std::runtime_error("injected sweep callback failure");
			}
		};
		const uint64_t iCallId = OI::Rpc::RpcCallManager::GetInstance().Bind(&stManager, 123456, 0, {}, &pSweepProbe->vCodes[iIndex], iRemaining, {},
		    std::move(fnCheckSweep), pSweepGuard);
		vCalls.push_back(iCallId);
	}
	const int64_t iSweepLimit = OI::Timer::SteadyMilliseconds() + 4000;
	while ((pSweepProbe->iCallbacks.load() != 128 || pSweepGuard->load() != 0) && OI::Timer::SteadyMilliseconds() < iSweepLimit)
	{
		co_await OI::Coro::yield(20);
	}
	for (const auto iCall : vCalls)
	{
		OI::Rpc::RpcCallManager::GetInstance().Abandon(iCall);
	}
	int iSweepBad = pSweepProbe->iCallbacks.load() == 128 && pSweepProbe->iFirstBatch.load() == 1 && pSweepGuard->load() == 0 && SameWaitStats(stBefore) ? 0 : 1;
	for (const auto iCode : pSweepProbe->vCodes)
	{
		if (iCode != OI_ERR_TIMEOUT)
		{
			++iSweepBad;
		}
	}
	iBad += iSweepBad;
	LOG_TRACE("hold sweep exception callbacks=%d same_batch=%d guard=%d bad=%d", pSweepProbe->iCallbacks.load(), pSweepProbe->iFirstBatch.load(), pSweepGuard->load(), iSweepBad);
	LOG_TRACE("hold absolute deadline buckets calls=4096 buckets=%zu fired=%d bad=%d", stDuring.iBuckets - stBefore.iBuckets, pProbe->iCompleted.load(), iBad);
	fflush(stdout);
	co_return iBad;
}

// 只在内部配对入口注入错误来源，不构造另一套网络客户端。
int TestRpcPending::CheckRpcSource(sid_t iSessionId)
{
	int iBadLocal = 0;
	auto& stManagerLocal = IpcClient::GetInstance();
	const auto iResponseIdLocal = ::OI::Rpc::RpcProtoId<::OI::RpcData::HoldRes>::iValue;
	int iDecoded = 0, iResumed = 0, iResultLocal = -99;
	uint64_t iCall = ::OI::Rpc::RpcCallManager::GetInstance().Bind(&stManagerLocal, iSessionId, iResponseIdLocal, {}, &iResultLocal, 0, [pDecoded = &iDecoded](::OI::Base::BytesStream&, sid_t)
	{
		++*pDecoded;
		return true;
	}, [&]()
	{
		++iResumed;
	});
	auto fnDeliver = [&](::OI::Rpc::ProtoSessionManager* pSource, sid_t iSourceSession)
	{
		::OI::Base::BytesStream stBody;
		stBody << iCall;
		::OI::Rpc::RpcCallManager::GetInstance().Deliver(iResponseIdLocal, stBody, pSource, iSourceSession);
	};
	fnDeliver(&stManagerLocal, iSessionId + 100000);
	fnDeliver(&::OI::Rpc::RouteManager::GetInstance(), iSessionId);
	if (iDecoded || iResumed)
	{
		++iBadLocal;
	}
	fnDeliver(&stManagerLocal, iSessionId);
	if (iDecoded != 1 || iResumed != 1)
	{
		++iBadLocal;
	}
	iResumed = 0;
	const sid_t iFakeSessionId = iSessionId + 100000;
	::OI::Rpc::RpcCallManager::GetInstance().Bind(&stManagerLocal, iFakeSessionId, iResponseIdLocal, {}, &iResultLocal, 0, {}, [&]()
	{
		++iResumed;
	});
	::OI::Rpc::RpcCallManager::GetInstance().FailSession(&stManagerLocal, iFakeSessionId);
	::OI::Rpc::RpcCallManager::GetInstance().FailSession(&stManagerLocal, iFakeSessionId);
	if (iResultLocal != OI_ERR_CONNECT || iResumed != 1)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold rpc source and disconnect bad=%d", iBadLocal);
	return iBadLocal;
}

// 建连前发送额度、回调上下文、待发条数和字节、4096 条会话索引、断连排空、提交异常、过期清理、绝对截止时间归桶，以及错误来源和断连唤醒。
OI::Coro::TaskResult<int> TestRpcPending::Run(sid_t iSessionId)
{
	int iBad = CheckPreconnectSendBudget();
	PendingClient stPending;
	iBad += co_await CheckRpcContext(stPending);
	iBad += CheckPendingLimits(stPending);
	iBad += CheckRpcSessionIndex(stPending);
	iBad += CheckRpcDisconnectDrain(stPending);
	iBad += co_await CheckRpcExceptions(stPending);
	iBad += co_await CheckBatchBindFailure(stPending);
	iBad += CheckRpcExpiry(stPending);
	iBad += co_await CheckDeadlineBuckets();
	iBad += CheckRpcSource(iSessionId);
	// 独立真实 TCP 连接：收到第一个回包时底层解码抛错，另一个尚在服务端等待。
	static PendingClient stWireClient;
	stWireClient.DefaultInit("127.0.0.1", HoldTest::IpcServerPort());
	const int32_t iConnectResult = co_await stWireClient.StartClient();
	struct WireProbe
	{
		int32_t vCode[2] = {-99, -99};
		std::atomic<int32_t> vResumed[2]{};
	};
	auto pWire = std::make_shared<WireProbe>();
	auto pWireGuard = std::make_shared<std::atomic<int32_t>>(0);
	std::vector<uint64_t> vWireCalls;
	const sid_t iWireSid = stWireClient.iProbeSession.load();
	const int32_t iDisconnectBefore = stWireClient.iProbeDisconnects.load();
	int iWireBad = iConnectResult == OI_OK && iWireSid != INVALID_SID ? 0 : 1;
	if (iWireBad == 0)
	{
		OI::Base::Bytes stWireFrames;
		for (int iIndex = 0; iIndex < 2; ++iIndex)
		{
			auto fnDecodeReply = [iIndex](OI::Base::BytesStream&, sid_t)
			{
				if (iIndex == 0)
				{
					throw std::runtime_error("injected response decode failure");
				}
				return true;
			};
			auto fnCountResume = [pWire, iIndex]()
			{
				pWire->vResumed[iIndex].fetch_add(1);
			};
			const uint64_t iCall = OI::Rpc::RpcCallManager::GetInstance().Bind(&stWireClient, iWireSid,
			    OI::Rpc::RpcProtoId<OI::RpcData::HoldRes>::iValue, {}, &pWire->vCode[iIndex], 3000,
			    std::move(fnDecodeReply), std::move(fnCountResume), pWireGuard);
			vWireCalls.push_back(iCall);
			OI::RpcData::HoldReq stRequest;
			stRequest.iDelayMilliseconds = iIndex == 0 ? 0 : 1000;
			OI::Base::BytesStream stBody;
			stBody << (iCall | uint64_t(RPC_REQ_MASK));
			stRequest.serialize(stBody);
			auto stFrame = OI::Rpc::EncodePbFrame(OI::Rpc::RpcProtoId<OI::RpcData::HoldReq>::iValue, stBody);
			stWireFrames.Append(stFrame.GetMutableBuff());
		}
		if (stWireClient.SendFrame(iWireSid, stWireFrames) != OI_OK)
		{
			++iWireBad;
		}
		const int64_t iUntil = OI::Timer::SteadyMilliseconds() + 4000;
		while ((pWire->vResumed[0].load() != 1 || pWire->vResumed[1].load() != 1 || stWireClient.iProbeDisconnects.load() == iDisconnectBefore || pWireGuard->load() != 0) && OI::Timer::SteadyMilliseconds() < iUntil)
		{
			co_await OI::Coro::yield(20);
		}
	}
	if (pWire->vResumed[0].load() != 1 || pWire->vCode[0] != OI_ERR_FAIL || pWire->vResumed[1].load() != 1 ||
	    pWire->vCode[1] != OI_ERR_CONNECT || stWireClient.iProbeDisconnects.load() != iDisconnectBefore + 1 || pWireGuard->load() != 0)
	{
		++iWireBad;
	}
	for (const auto iCall : vWireCalls)
	{
		OI::Rpc::RpcCallManager::GetInstance().Abandon(iCall);
	}
	stWireClient.CloseSession(iWireSid);
	LOG_TRACE("hold rpc receive exception first=%d/%d second=%d/%d close=%d guard=%d bad=%d", pWire->vResumed[0].load(), pWire->vCode[0], pWire->vResumed[1].load(), pWire->vCode[1], stWireClient.iProbeDisconnects.load() - iDisconnectBefore, pWireGuard->load(), iWireBad);
	iBad += iWireBad;
	co_return iBad;
}
