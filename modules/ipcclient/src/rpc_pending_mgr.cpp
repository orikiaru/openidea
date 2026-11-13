#include "rpc_pending_mgr.h"
#include "hold_test.h"
#include "oi_rpc_coro.h"
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


// 使用真实待连接队列验证条数、字节限制以及取消后的容量恢复。
struct RpcPendingMgr::PendingClient : OI::Rpc::ProtoClientManager
{
	int iFailureMode = 0;
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
	void OnClientConnect(OI::Net::Session*) override
	{
	}
	void OnClientDisconnect(const OI::Net::Session*) override
	{
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
			OI::Rpc::WakeRpcCall(iId, OI_ERR_FAIL, nullptr);
		}
		if (iFailureMode == 3)
		{
			ProtoClientManager::TryPendCall(iId, stData);
		}
		if (iFailureMode == 4)
		{
			OI::Coro::StartTask([iId](OI::Coro::Token) -> OI::Coro::Task
			{
				OI::Rpc::WakeRpcCall(iId, OI_ERR_FAIL, nullptr);
				co_return;
			});
		}
		throw std::runtime_error("injected submission failure");
	}
};

bool RpcPendingMgr::SameWaitStats(const OI::Rpc::RpcWaitStats& stBaseline)
{
	const auto stNow = OI::Rpc::GetRpcWaitStats();
	return stNow.iCalls == stBaseline.iCalls
		&& stNow.iExpirations == stBaseline.iExpirations
		&& stNow.iBuckets == stBaseline.iBuckets
		&& stNow.iSessionEntries == stBaseline.iSessionEntries
		&& stNow.iSessionBuckets == stBaseline.iSessionBuckets;
}

int RpcPendingMgr::CheckPreconnectSendBudget()
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
	LOG_TRACE("hold preconnect send budget bad=%d", iBadLocal);
	return iBadLocal;
}

// 标准回调的可选上下文跨挂起保持有效。
OI::Coro::TaskResult<int> RpcPendingMgr::CheckRpcContext(PendingClient& stPending)
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
	co_await OI::Rpc::RunHandledRpc<Req, Res>(fnContextHandler, OI::Rpc::RpcProtoId<Res>::iValue, 88, &stPending, 77, std::move(stBody));
	if (iCallbacks != 1)
	{
		++iBadLocal;
	}
	LOG_TRACE("hold rpc context bad=%d", iBadLocal);
	co_return iBadLocal;
}

uint64_t RpcPendingMgr::BindPendingCall(PendingClient& stPending, int32_t* pResult, int64_t iTimeoutMilliseconds)
{
	return OI::Rpc::BindRpcCall(&stPending, INVALID_SID, 0, {}, pResult, iTimeoutMilliseconds, {}, []()
	{
	});
}

int RpcPendingMgr::CheckPendingLimits(PendingClient& stPending)
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
	OI::Rpc::WakeRpcCall(iFirstCall, OI_ERR_TIMEOUT, nullptr);
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
int RpcPendingMgr::CheckRpcSessionIndex(PendingClient& stPending)
{
	int iBadLocal = 0;
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
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
	if (!SameWaitStats(stBaseline))
	{
		++iBadLocal;
	}
	LOG_TRACE("hold rpc session index calls=4096 bad=%d", iBadLocal);
	return iBadLocal;
}

// 跨分片、跨批次断连清理：回调抛异常也必须完成后续项并归还所有 guard。
int RpcPendingMgr::CheckRpcDisconnectDrain(PendingClient& stPending)
{
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
	std::vector<int32_t> vResults(4096, -99);
	auto pGuard = std::make_shared<std::atomic<int32_t>>(0);
	auto pCallbacks = std::make_shared<std::atomic<int32_t>>(0);
	for (size_t iIndex = 0; iIndex < vResults.size(); ++iIndex)
	{
		OI::Rpc::BindRpcCall(&stPending, 987654, 0, {}, &vResults[iIndex], 180000, {}, [pCallbacks]()
		{
			if (pCallbacks->fetch_add(1) == 0)
			{
				throw std::bad_alloc();
			}
		}, pGuard);
	}
	OI::Rpc::FailRpcCalls(&stPending, 987654);
	int iBad = pCallbacks->load() == 4096 && pGuard->load() == 0 && SameWaitStats(stBaseline) ? 0 : 1;
	for (const auto& iResult : vResults)
	{
		if (iResult != OI_ERR_CONNECT)
		{
			++iBad;
		}
	}
	LOG_TRACE("hold disconnect drain calls=4096 bad=%d", iBad);
	return iBad;
}

// 登记后序列化异常，以及发送异常与完成回调竞争；恢复后不能留下悬空等待项。
OI::Coro::TaskResult<int> RpcPendingMgr::CheckRpcExceptions(PendingClient& stPending)
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

// 每个分片留一条长超时请求堵住队首，再拒绝十万条；索引数量必须保持不变。
int RpcPendingMgr::CheckRpcExpiry(PendingClient& stPending)
{
	int iBadLocal = 0;
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
	OI::Base::BytesStream stFrame;
	stFrame << uint64_t(1);
	stPending.SetPendingCallLimits(64, 64 * 8);
	int32_t vAnchorResults[64]{};
	for (auto& iAnchorResult : vAnchorResults)
	{
		stPending.TryPendCall(BindPendingCall(stPending, &iAnchorResult, 180000), stFrame.GetMutableBuff());
	}
	const auto stAnchored = OI::Rpc::GetRpcWaitStats();
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
	const auto stLimited = OI::Rpc::GetRpcWaitStats();
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

OI::Coro::TaskResult<int> RpcPendingMgr::CheckDeadlineBuckets()
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
		co_await OI::Coro::yield(20);
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
	LOG_TRACE("hold absolute deadline buckets calls=4096 buckets=%zu fired=%d bad=%d", stDuring.iBuckets - stBefore.iBuckets, pProbe->iCompleted.load(), iBad);
	fflush(stdout);
	co_return iBad;
}

// 只在内部配对入口注入错误来源，不构造另一套网络客户端。
int RpcPendingMgr::CheckRpcSource(sid_t iSessionId)
{
	int iBadLocal = 0;
	auto& stManagerLocal = IpcClient::GetInstance();
	const auto iResponseIdLocal = ::OI::Rpc::RpcProtoId<::OI::RpcData::HoldRes>::iValue;
	int iDecoded = 0, iResumed = 0, iResultLocal = -99;
	uint64_t iCall = ::OI::Rpc::BindRpcCall(&stManagerLocal, iSessionId, iResponseIdLocal, {}, &iResultLocal, 0, [pDecoded = &iDecoded](::OI::Base::BytesStream&, sid_t)
	{
		++*pDecoded;
		return true;
	}, [&]()
	{
		++iResumed;
	});
	auto fnDeliver = [&](::OI::Net::SessionManager* pSource, sid_t iSourceSession)
	{
		::OI::Base::BytesStream stBody;
		stBody << iCall;
		::OI::Rpc::DeliverRpc(iResponseIdLocal, stBody, pSource, iSourceSession);
	};
	fnDeliver(&stManagerLocal, iSessionId + 100000);
	fnDeliver(&::OI::Redis::RedisClient::GetInstance(), iSessionId);
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
	LOG_TRACE("hold rpc source and disconnect bad=%d", iBadLocal);
	return iBadLocal;
}

// 建连前发送额度、回调上下文、待发条数和字节、4096 条会话索引、断连排空、提交异常、过期清理、绝对截止时间归桶，以及错误来源和断连唤醒。
OI::Coro::TaskResult<int> RpcPendingMgr::Run(sid_t iSessionId)
{
	int iBad = CheckPreconnectSendBudget();
	PendingClient stPending;
	iBad += co_await CheckRpcContext(stPending);
	iBad += CheckPendingLimits(stPending);
	iBad += CheckRpcSessionIndex(stPending);
	iBad += CheckRpcDisconnectDrain(stPending);
	iBad += co_await CheckRpcExceptions(stPending);
	iBad += CheckRpcExpiry(stPending);
	iBad += co_await CheckDeadlineBuckets();
	iBad += CheckRpcSource(iSessionId);
	co_return iBad;
}
