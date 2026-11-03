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
	coro::StartTask([](coro::Token) {
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
		std::chrono::steady_clock::now() - stBeginParam).count();
}

static bool Pong(const ::OI::Redis::RedisResult& stResultParam)
{
	return !stResultParam.bTimeout && stResultParam.pReply && stResultParam.pReply->type == REDIS_REPLY_STATUS
		&& stResultParam.pReply->str && std::strcmp(stResultParam.pReply->str, "PONG") == 0;
}

struct RaceBox
{
	int32_t vReturnCodes[8];
	int32_t vMarks[8];
	std::atomic<int32_t> iFinished;
	RaceBox() : iFinished(0)
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

coro::Task HoldMgr::Pump()
{
	int iBadLocal = 0;
	// 发送预算必须跨会话/通道缓冲连续计数，真正写出才恢复额度。
	struct BufferedSession : OI::Net::NetSession
	{
		BufferedSession() : NetSession(nullptr) {}
		void OnDataIn() override {}
		void OnDataOut() override {}
	} stBuffered;
	stBuffered.SetSendQueueLimit(8);
	OI::Base::Bytes stPayload("12345678", 8), stStaged;
	if (!stBuffered.Send(stPayload) || stBuffered.Send(stPayload)) ++iBadLocal;
	stBuffered.SwapOutBuff(stStaged);
	if (stBuffered.PendingSendBytes() != 8 || stBuffered.Send(stPayload)) ++iBadLocal;
	stBuffered.OnBytesSent(8);
	stStaged.Clear();
	if (!stBuffered.Send(stPayload)) ++iBadLocal;
	stBuffered.ActiveClose();
	if (!stBuffered.IsClosed() || stBuffered.Send(stPayload)) ++iBadLocal;
	printf("hold send budget bad=%d\n", iBadLocal);

	// 持续就绪的协程不能饿死同优先级普通任务。
	std::atomic<bool> bOrdinaryDone{false};
	coro::TaskScope stBusyScope;
	const auto stFairnessBegin = std::chrono::steady_clock::now();
	for (int iIndexLocal = 0; iIndexLocal < 64; ++iIndexLocal)
		stBusyScope.Start([&bOrdinaryDone, stFairnessBegin](coro::Token, coro::StopToken stStopParam) -> coro::Task {
			while (!bOrdinaryDone.load() && !stStopParam.StopRequested()
				&& std::chrono::steady_clock::now() - stFairnessBegin < OI::Timer::Seconds(3))
				co_await coro::yield(0);
		});
	co_await coro::yield(OI::Timer::Milliseconds(50));
	if (!OI::Thread::ThreadPool::GetInstance().AddTask([&bOrdinaryDone]() { bOrdinaryDone.store(true); })) { ++iBadLocal; bOrdinaryDone.store(true); }
	while (!bOrdinaryDone.load()) co_await coro::yield(0);
	if (WallMs(stFairnessBegin) > 1500) ++iBadLocal;
	stBusyScope.RequestStop();
	co_await stBusyScope.Join();
	printf("hold scheduler fairness bad=%d\n", iBadLocal);

	// 子任务异常跨挂起点传回父任务；作用域关闭后不再接受任务。
	auto fnThrowing = [](coro::Token, coro::StopToken) -> coro::Task {
		co_await coro::yield(::OI::Timer::Milliseconds(10));
		throw std::runtime_error("scope regression");
	};
	coro::TaskScope stFailures;
	stFailures.Start(fnThrowing);
	bool bCaught = false;
	try { co_await stFailures.Join(); }
	catch (const std::runtime_error&) { bCaught = true; }
	if (!bCaught) ++iBadLocal;
	coro::TaskScope stScope;
	std::atomic<int> iFinishedLocal{0}, iStarted{0};
	for (int iIndexLocal = 0; iIndexLocal < 128; ++iIndexLocal)
		stScope.Start([&iFinishedLocal, &iStarted](coro::Token, coro::StopToken stStopParam) -> coro::Task {
			++iStarted;
			while (!stStopParam.StopRequested()) co_await coro::yield(::OI::Timer::Milliseconds(10));
			++iFinishedLocal;
		});
	while (iStarted.load() != 128) co_await coro::yield(::OI::Timer::Milliseconds(10));
	stScope.RequestStop();
	co_await stScope.Join();
	if (iFinishedLocal.load() != 128) ++iBadLocal;
	try { stScope.Start(fnThrowing); ++iBadLocal; } catch (const std::logic_error&) {}
	printf("hold scope finished=%d caught=%d bad=%d\n", iFinishedLocal.load(), bCaught, iBadLocal);
	// 真正的带捕获协程 lambda：临时闭包离开后、挂起恢复后仍访问捕获。
	struct Captures { std::atomic<int> iDone{0}; std::atomic<int> iBad{0}; };
	auto pCaptures = std::make_shared<Captures>();
	for (int iIndexLocal = 0; iIndexLocal < 32; ++iIndexLocal)
	{
		coro::StartTask([pCaptures, text = std::string(4096, 'a' + iIndexLocal % 26), iIndexLocal](coro::Token iTokenParam) -> coro::Task {
			co_await coro::yield(::OI::Timer::Milliseconds(0));
			co_await coro::yield(::OI::Timer::Milliseconds(10));
			if (!iTokenParam || text != std::string(4096, 'a' + iIndexLocal % 26)) pCaptures->iBad.fetch_add(1);
			pCaptures->iDone.fetch_add(1, std::memory_order_release);
		});
	}
	while (pCaptures->iDone.load(std::memory_order_acquire) < 32) co_await coro::yield(::OI::Timer::Milliseconds(10));
	iBadLocal += pCaptures->iBad.load();
	printf("hold captured tasks done=%d bad=%d\n", pCaptures->iDone.load(), pCaptures->iBad.load());

	// 使用真实待连接队列验证条数、字节限制以及取消后的容量恢复。
	struct PendingClient : OI::Rpc::ProtoClientManager
	{
		int iFailureMode = 0;
		void SetSendLimit(size_t iBytesParam) { iMaxSendQueueBytes = iBytesParam; }
		OI::Rpc::ProtoSession* AttachForTest()
		{
			auto pSessionLocal = new OI::Rpc::ProtoSession(this);
			AddSession(pSessionLocal);
			return pSessionLocal;
		}
		void OnClientConnect(OI::Net::Session*) override {}
		void OnClientDisconnect(const OI::Net::Session*) override {}
		bool TryPendCall(uint64_t iIdParam, const OI::Base::Bytes& stDataParam) override
		{
			if (!iFailureMode) return ProtoClientManager::TryPendCall(iIdParam, stDataParam);
			if (iFailureMode == 2) OI::Rpc::WakeRpcCall(iIdParam, OI_ERR_FAIL, nullptr);
			if (iFailureMode == 3) ProtoClientManager::TryPendCall(iIdParam, stDataParam);
			if (iFailureMode == 4)
				coro::StartTask([iIdParam](coro::Token) -> coro::Task {
					OI::Rpc::WakeRpcCall(iIdParam, OI_ERR_FAIL, nullptr);
					co_return;
				});
			throw std::runtime_error("injected submission failure");
		}
	} stPending;
	{
		PendingClient stRawPending;
		stRawPending.SetSendLimit(8);
		if (!stRawPending.SendRawOrPend(stPayload, 1024) || stRawPending.SendRawOrPend(stPayload, 1024)) ++iBadLocal;
		auto pSessionLocal = stRawPending.AttachForTest();
		if (pSessionLocal->PendingSendBytes() != 8 || stRawPending.SendRaw(stPayload)) ++iBadLocal;
		stRawPending.CloseSession(pSessionLocal->GetSessionID());
		pSessionLocal->Release();
	}
	printf("hold preconnect send budget bad=%d\n", iBadLocal);
	// 可选上下文跨挂起保持有效，旧五参数无返回值处理函数仍可注册。
	{
		using Req = OI::RpcData::HoldReq;
		using Res = OI::RpcData::HoldRes;
		auto fnBody = []() {
			OI::Base::BytesStream stBytes;
			stBytes << (uint64_t(1) | uint64_t(RPC_REQ_MASK));
			Req stRequestLocal;
			stRequestLocal.iMark = 73;
			stRequestLocal.serialize(stBytes);
			return stBytes;
		};
		int iCallbacks = 0;
		auto fnContextHandler = [&](const OI::Rpc::RpcContext& stContextParam, const Req& stRequestParam, Res&) -> OI::Rpc::RpcTask {
			co_await coro::yield(0);
			if (stContextParam.iToken != 88 || &stContextParam.stManager != &stPending || stContextParam.iSessionId != 77 || stRequestParam.iMark != 73) ++iBadLocal;
			++iCallbacks;
			co_return 1001;
		};
		co_await OI::Rpc::RunHandledRpc<Req, Res>(fnContextHandler, OI::Rpc::RpcProtoId<Res>::iValue, 88, &stPending, 77, fnBody());
		auto fnLegacyHandler = [&](coro::Token iTokenParam, OI::Net::SessionManager* pManagerParam, sid_t iSessionIdParam, Req&, Res&) -> coro::Task {
			if (iTokenParam != 88 || pManagerParam != &stPending || iSessionIdParam != 77) ++iBadLocal;
			++iCallbacks;
			co_return;
		};
		co_await OI::Rpc::RunHandledRpc<Req, Res>(fnLegacyHandler, OI::Rpc::RpcProtoId<Res>::iValue, 88, &stPending, 77, fnBody());
		if (iCallbacks != 2) ++iBadLocal;
	}

	stPending.SetPendingCallLimits(1, 8);
	OI::Base::BytesStream stFrame;
	stFrame << uint64_t(1);
	int iFirstFirst = 0, iSecondSecond = 0;
	auto fnBind = [&](int& iResultParam) {
		return OI::Rpc::BindRpcCall(&stPending, INVALID_SID, 0, {}, &iResultParam, 0, {}, []() {});
	};
	auto iFirstA = fnBind(iFirstFirst);
	stPending.TryPendCall(iFirstA, stFrame.GetMutableBuff());
	auto iSecondB = fnBind(iSecondSecond);
	stPending.TryPendCall(iSecondB, stFrame.GetMutableBuff());
	if (iSecondSecond != OI_ERR_BUSY) ++iBadLocal;
	OI::Rpc::WakeRpcCall(iFirstA, OI_ERR_TIMEOUT, nullptr);
	iSecondSecond = 0;
	iSecondB = fnBind(iSecondSecond);
	stPending.TryPendCall(iSecondB, stFrame.GetMutableBuff());
	if (iSecondSecond != 0) ++iBadLocal;
	stPending.FailHeldCalls();
	if (iSecondSecond != OI_ERR_CONNECT) ++iBadLocal;
	stPending.SetPendingCallLimits(8, 7);
	iSecondSecond = 0;
	iSecondB = fnBind(iSecondSecond);
	stPending.TryPendCall(iSecondB, stFrame.GetMutableBuff());
	if (iSecondSecond != OI_ERR_BUSY) ++iBadLocal;
	printf("hold pending limits bad=%d\n", iBadLocal);

	// 登记后序列化异常，以及发送异常与完成回调竞争；恢复后不能留下悬空等待项。
	const auto stBaseline = OI::Rpc::GetRpcWaitStats();
	auto fnBalanced = [&]() {
		const auto stNow = OI::Rpc::GetRpcWaitStats();
		return stNow.iCalls == stBaseline.iCalls && stNow.iExpirations == stBaseline.iExpirations
			&& stNow.iBuckets == stBaseline.iBuckets;
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
	try { co_await OI::Rpc::RpcCall(stPending, INVALID_SID, stThrowingBase, stUnused, OI::Timer::Milliseconds(50)); }
	catch (const std::runtime_error&) { ++iCaughtErrors; }
	if (!fnBalanced()) ++iBadLocal;
	stPending.SetPendingCallLimits(64, 64 * 1024 * 1024);
	OI::RpcData::HoldReq stOrdinary;
	for (int iMode = 1; iMode <= 4; ++iMode)
	{
		stPending.iFailureMode = iMode;
		for (int iIndexLocal = 0; iIndexLocal < 128; ++iIndexLocal)
		{
			try { co_await OI::Rpc::RpcCall(stPending, INVALID_SID, stOrdinary, stUnused, OI::Timer::Milliseconds(50)); }
			catch (const std::runtime_error&) { ++iCaughtErrors; }
			if (!fnBalanced()) ++iBadLocal;
		}
	}
	stPending.iFailureMode = 0;
	co_await coro::yield(OI::Timer::Milliseconds(250));
	if (iCaughtErrors != 513 || !fnBalanced()) ++iBadLocal;
	printf("hold rpc exceptions caught=%d bad=%d\n", iCaughtErrors, iBadLocal);

	// 每个分片留一条长超时请求堵住队首，再拒绝十万条；索引数量必须保持不变。
	stPending.SetPendingCallLimits(64, 64 * 8);
	int vAnchorResults[64]{};
	for (int iIndexLocal = 0; iIndexLocal < 64; ++iIndexLocal)
	{
		auto iIdLocal = OI::Rpc::BindRpcCall(&stPending, INVALID_SID, 0, {}, &vAnchorResults[iIndexLocal], 180000, {}, []() {});
		stPending.TryPendCall(iIdLocal, stFrame.GetMutableBuff());
	}
	const auto stAnchored = OI::Rpc::GetRpcWaitStats();
	if (stAnchored.iCalls != stBaseline.iCalls + 64 || stAnchored.iExpirations != stBaseline.iExpirations + 64) ++iBadLocal;
	for (int iIndexLocal = 0; iIndexLocal < 100000; ++iIndexLocal)
	{
		int iRejected = 0;
		auto iIdLocal = OI::Rpc::BindRpcCall(&stPending, INVALID_SID, 0, {}, &iRejected, 180000 + iIndexLocal % 17, {}, []() {});
		stPending.TryPendCall(iIdLocal, stFrame.GetMutableBuff());
		if (iRejected != OI_ERR_BUSY) ++iBadLocal;
	}
	const auto stLimited = OI::Rpc::GetRpcWaitStats();
	if (stLimited.iCalls != stAnchored.iCalls || stLimited.iExpirations != stAnchored.iExpirations
		|| stLimited.iBuckets != stAnchored.iBuckets) ++iBadLocal;
	stPending.FailHeldCalls();
	for (int iCodeLocal : vAnchorResults) if (iCodeLocal != OI_ERR_CONNECT) ++iBadLocal;
	if (!fnBalanced()) ++iBadLocal;
	printf("hold rpc expiry rejected=100000 retained=%zu bad=%d\n", stLimited.iExpirations - stBaseline.iExpirations, iBadLocal);

	// 只在内部配对入口注入错误来源，不构造另一套网络客户端。
	auto& stManagerLocal = IpcClient::GetInstance();
	const auto iResponseIdLocal = ::OI::Rpc::RpcProtoId<::OI::RpcData::HoldRes>::iValue;
	int iDecoded = 0, iResumed = 0, iResultLocal = -99;
	uint64_t iCall = ::OI::Rpc::BindRpcCall(&stManagerLocal, m_iSessionId, iResponseIdLocal, {}, &iResultLocal, 0,
		[&](::OI::Base::BytesStream&) { ++iDecoded; return true; }, [&]() { ++iResumed; });
	auto fnDeliver = [&](::OI::Net::SessionManager* pSourceParam, sid_t iSessionIdParam) {
		::OI::Base::BytesStream stBody;
		stBody << iCall;
		::OI::Rpc::DeliverRpc(iResponseIdLocal, stBody, pSourceParam, iSessionIdParam);
	};
	fnDeliver(&stManagerLocal, m_iSessionId + 100000);
	fnDeliver(&::OI::Redis::RedisClient::GetInstance(), m_iSessionId);
	if (iDecoded || iResumed) ++iBadLocal;
	fnDeliver(&stManagerLocal, m_iSessionId);
	if (iDecoded != 1 || iResumed != 1) ++iBadLocal;
	iResumed = 0;
	const sid_t iFakeSessionId = m_iSessionId + 100000;
	::OI::Rpc::BindRpcCall(&stManagerLocal, iFakeSessionId, iResponseIdLocal, {}, &iResultLocal, 0, {}, [&]() { ++iResumed; });
	::OI::Rpc::FailRpcCalls(&stManagerLocal, iFakeSessionId);
	::OI::Rpc::FailRpcCalls(&stManagerLocal, iFakeSessionId);
	if (iResultLocal != OI_ERR_CONNECT || iResumed != 1) ++iBadLocal;
	printf("hold rpc source and disconnect bad=%d\n", iBadLocal);

	::OI::RpcData::HoldReq stRequestLocal;
	::OI::RpcData::HoldRes stResponseLocal;

	stRequestLocal.iDelayMilliseconds = 0;
	stRequestLocal.iMark = 1;
	auto stDirect = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, ::OI::Timer::Seconds(10));
	if (!stDirect || stDirect.GetRes().iMark != 1) ++iBadLocal;
	static_assert(std::is_same_v<decltype(stDirect.GetRet()), int32_t>);
	stRequestLocal.iDelayMilliseconds = -1;
	auto stFailed = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal);
	if (stFailed.GetRet() != 1001 || stFailed) ++iBadLocal;
	bool bGuarded = false;
	try { (void)stFailed.GetRes(); }
	catch (const std::logic_error&) { bGuarded = true; }
	if (!bGuarded) ++iBadLocal;
	stResponseLocal.iMark = 876;
	int32_t iBusinessReturnCode = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, stResponseLocal);
	if (iBusinessReturnCode != 1001 || stResponseLocal.iMark != 876) ++iBadLocal;
	stRequestLocal.iDelayMilliseconds = 0;
	iBusinessReturnCode = co_await ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, stResponseLocal, ::OI::Timer::Seconds(10));
	if (iBusinessReturnCode != 0 || stResponseLocal.iMark != 1) ++iBadLocal;
	// 缺状态码和截断响应都应返回框架错误，不能覆盖应用已有响应。
	for (int iMalformed = 0; iMalformed < 2; ++iMalformed)
	{
		stResponseLocal.iMark = 876;
		auto stBroken = ::OI::Rpc::RpcCall(stManagerLocal, stRequestLocal, stResponseLocal);
		stBroken.bReplied = true;
		if (iMalformed) stBroken.stReply << int32_t(0) << uint8_t(1);
		if (stBroken.await_resume() != OI_ERR_FAIL || stResponseLocal.iMark != 876) ++iBadLocal;
	}
	auto fnValueTask = []() -> coro::TaskResult<int32_t> {
		co_await coro::yield(0);
		co_return 77;
	};
	if (co_await fnValueTask() != 77) ++iBadLocal;
	auto fnThrowingTask = []() -> coro::TaskResult<int32_t> {
		co_await coro::yield(0);
		throw std::runtime_error("result task test");
		co_return 0;
	};
	bool bPropagated = false;
	try { (void)co_await fnThrowingTask(); }
	catch (const std::runtime_error&) { bPropagated = true; }
	if (!bPropagated) ++iBadLocal;
	printf("hold rpc standardized reply bad=%d\n", iBadLocal);
	stRequestLocal.iDelayMilliseconds = 600;
	auto stShortWait = co_await ::OI::Rpc::RpcCall<::OI::RpcData::HoldRes>(stManagerLocal, m_iSessionId, stRequestLocal, ::OI::Timer::Milliseconds(150));
	if (stShortWait.GetRet() != OI_ERR_TIMEOUT) ++iBadLocal;
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
		coro::StartTask([iMark, iDelay, iIndexLocal, pBox, iSessionIdLocal](coro::Token) {
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
	coro::StartTask([iCloseSessionId](coro::Token) -> coro::Task {
		co_await coro::yield(::OI::Timer::Milliseconds(100));
		IpcClient::GetInstance().CloseSession(iCloseSessionId);
	});
	stWallBegin = std::chrono::steady_clock::now();
	auto stClosed = co_await ::OI::Rpc::RpcCall<::OI::RpcData::HoldRes>(stManagerLocal, m_iSessionId, stRequestLocal, ::OI::Timer::Seconds(10));
	if (stClosed.GetRet() != OI_ERR_CONNECT || WallMs(stWallBegin) > 2000) ++iBadLocal;
	printf("hold rpc disconnected code=%d wall_ms=%lld\n", stClosed.GetRet(), WallMs(stWallBegin));
	printf("hold done bad=%d\n", iBadLocal);
	fflush(stdout);
	TestBoard::GetInstance().Done(iBadLocal == 0 ? 0 : 1);
	co_return;
}
