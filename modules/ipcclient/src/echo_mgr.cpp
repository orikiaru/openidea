#include "echo_mgr.h"
#include "oi_config.h"
#include "oi_log.h"
#include "ipcclient.h"
#include "rpc_ids.h"
#include "test_board.h"
#include "oi_timer_manager.h"
#include "oi_thread.h"
#include <cstdio>
#include <cstring>
#include <string>
#include <memory>
#include <vector>

#ifdef _SINGLE_THREAD_
static const char* kMode = "st";
#else
static const char* kMode = "mt";
#endif

static bool SameBytes(const ::OI::Base::Bytes& stFirstParam, const ::OI::Base::Bytes& stSecondParam)
{
	if (stFirstParam.Size() != stSecondParam.Size())
	{
		return false;
	}
	if (stFirstParam.Size() == 0)
	{
		return true;
	}
	return memcmp(stFirstParam.Data(), stSecondParam.Data(), stFirstParam.Size()) == 0;
}

static bool SameInner(const ::OI::RpcData::Inner& stFirstParam, const ::OI::RpcData::Inner& stSecondParam)
{
	return stFirstParam.iNumber == stSecondParam.iNumber && stFirstParam.sText == stSecondParam.sText;
}

static void FillAllTypes(::OI::RpcData::AllTypesData& stDataParam, int iNumberParam)
{
	stDataParam.iInt8 = (int8_t)(iNumberParam & 0x7f);
	stDataParam.iInt16 = (int16_t)(1000 + iNumberParam);
	stDataParam.iInt32 = iNumberParam;
	stDataParam.iInt64 = 100000LL + iNumberParam;
	stDataParam.iAlternateInt64 = 10000000000LL + iNumberParam;
	stDataParam.fFloat = 1.5f + (float)iNumberParam;
	stDataParam.dDouble = 2.5 + iNumberParam;
	stDataParam.stRawData.Clear();
	char vRaw[4];
	memcpy(vRaw, &iNumberParam, 4);
	stDataParam.stRawData.Append(vRaw, 4);
	stDataParam.sMessage = "msg-" + std::to_string(iNumberParam);
	stDataParam.vNumbers.clear();
	stDataParam.vNumbers.push_back(iNumberParam);
	stDataParam.vNumbers.push_back(iNumberParam + 1);
	stDataParam.vWords.clear();
	stDataParam.vWords.push_back("w");
	stDataParam.vWords.push_back(std::to_string(iNumberParam));
	stDataParam.stChild.iNumber = iNumberParam;
	stDataParam.stChild.sText = "child";
	stDataParam.vChildren.clear();
	stDataParam.vChildren.push_back(stDataParam.stChild);
	::OI::RpcData::Inner stKin;
	stKin.iNumber = iNumberParam + 7;
	stKin.sText = "kin";
	stDataParam.vChildren.push_back(stKin);
	stDataParam.mTable.clear();
	stDataParam.mTable[iNumberParam] = "t";
	stDataParam.mTable[iNumberParam + 1] = "u";
	stDataParam.mNamed.clear();
	stDataParam.mNamed["a"] = stDataParam.stChild;
	stDataParam.mNamed["b"] = stKin;
	stDataParam.iUnsigned8 = (uint8_t)200;
	stDataParam.iUnsigned16 = (uint16_t)40000;
	stDataParam.iUnsigned32 = 3000000000u + (uint32_t)(iNumberParam & 0xff);
	stDataParam.iUnsigned64 = (1ULL << 63) + (uint64_t)iNumberParam;
	stDataParam.vUnsignedNumbers.clear();
	stDataParam.vUnsignedNumbers.push_back(stDataParam.iUnsigned64);
	stDataParam.vUnsignedNumbers.push_back(1);
	stDataParam.iGap = 20 + iNumberParam;
	stDataParam.mUnsignedTable.clear();
	stDataParam.mUnsignedTable[stDataParam.iUnsigned32] = "u";
	stDataParam.mLargeUnsignedTable.clear();
	stDataParam.mLargeUnsignedTable[stDataParam.iUnsigned64] = "b";
	stDataParam.mKeyed.clear();
	::OI::RpcData::EchoKey stKey;
	stKey.stPart.iNumber = iNumberParam;
	stKey.sName = "k";
	stDataParam.mKeyed[stKey] = "v";
	stKey.stPart.iNumber = iNumberParam + 1;
	stKey.sName = "k2";
	stDataParam.mKeyed[stKey] = "w";
}

static void PoisonAllTypes(::OI::RpcData::AllTypesData& stDataParam)
{
	stDataParam.iInt8 = 0x5a;
	stDataParam.iInt16 = -1;
	stDataParam.iInt32 = -2;
	stDataParam.iInt64 = -3;
	stDataParam.iAlternateInt64 = -4;
	stDataParam.fFloat = -5.0f;
	stDataParam.dDouble = -6.0;
	stDataParam.stRawData.Clear();
	stDataParam.sMessage = "poison";
	stDataParam.vNumbers.clear();
	stDataParam.vWords.clear();
	stDataParam.stChild.iNumber = -7;
	stDataParam.stChild.sText = "poison";
	stDataParam.vChildren.clear();
	stDataParam.mTable.clear();
	stDataParam.mNamed.clear();
	stDataParam.iUnsigned8 = 1;
	stDataParam.iUnsigned16 = 1;
	stDataParam.iUnsigned32 = 1;
	stDataParam.iUnsigned64 = 1;
	stDataParam.vUnsignedNumbers.clear();
	stDataParam.iGap = -12345;
	stDataParam.mUnsignedTable.clear();
	stDataParam.mLargeUnsignedTable.clear();
	stDataParam.mKeyed.clear();
}

static bool DiffAllTypes(const ::OI::RpcData::AllTypesData& stSentParam, const ::OI::RpcData::AllTypesData& stRecvParam)
{
	bool bSame = true;
	if (stSentParam.iInt8 != stRecvParam.iInt8)
	{
		EchoMgr::GetInstance().Note("c");
		bSame = false;
	}
	if (stSentParam.iInt16 != stRecvParam.iInt16)
	{
		EchoMgr::GetInstance().Note("s");
		bSame = false;
	}
	if (stSentParam.iInt32 != stRecvParam.iInt32)
	{
		EchoMgr::GetInstance().Note("i");
		bSame = false;
	}
	if (stSentParam.iInt64 != stRecvParam.iInt64)
	{
		EchoMgr::GetInstance().Note("l");
		bSame = false;
	}
	if (stSentParam.iAlternateInt64 != stRecvParam.iAlternateInt64)
	{
		EchoMgr::GetInstance().Note("i64");
		bSame = false;
	}
	if (stSentParam.fFloat != stRecvParam.fFloat)
	{
		EchoMgr::GetInstance().Note("f");
		bSame = false;
	}
	if (stSentParam.dDouble != stRecvParam.dDouble)
	{
		EchoMgr::GetInstance().Note("d");
		bSame = false;
	}
	if (!SameBytes(stSentParam.stRawData, stRecvParam.stRawData))
	{
		EchoMgr::GetInstance().Note("raw");
		bSame = false;
	}
	if (stSentParam.sMessage != stRecvParam.sMessage)
	{
		EchoMgr::GetInstance().Note("msg");
		bSame = false;
	}
	if (stSentParam.vNumbers != stRecvParam.vNumbers)
	{
		EchoMgr::GetInstance().Note("nums");
		bSame = false;
	}
	if (stSentParam.vWords != stRecvParam.vWords)
	{
		EchoMgr::GetInstance().Note("words");
		bSame = false;
	}
	if (!SameInner(stSentParam.stChild, stRecvParam.stChild))
	{
		EchoMgr::GetInstance().Note("child");
		bSame = false;
	}
	if (stSentParam.vChildren.size() != stRecvParam.vChildren.size())
	{
		EchoMgr::GetInstance().Note("children");
		bSame = false;
	}
	else
	{
		for (size_t iIndexLocal = 0; iIndexLocal < stSentParam.vChildren.size(); ++iIndexLocal)
		{
			if (!SameInner(stSentParam.vChildren[iIndexLocal], stRecvParam.vChildren[iIndexLocal]))
			{
				EchoMgr::GetInstance().Note("children");
				bSame = false;
				break;
			}
		}
	}
	if (stSentParam.mTable != stRecvParam.mTable)
	{
		EchoMgr::GetInstance().Note("table");
		bSame = false;
	}
	if (stSentParam.mNamed.size() != stRecvParam.mNamed.size())
	{
		EchoMgr::GetInstance().Note("named");
		bSame = false;
	}
	else
	{
		for (const auto& [sName, stValue] : stSentParam.mNamed)
		{
			auto stJt = stRecvParam.mNamed.find(sName);
			if (stJt == stRecvParam.mNamed.end() || !SameInner(stValue, stJt->second))
			{
				EchoMgr::GetInstance().Note("named");
				bSame = false;
				break;
			}
		}
	}
	if (stSentParam.iUnsigned8 != stRecvParam.iUnsigned8)
	{
		EchoMgr::GetInstance().Note("u8");
		bSame = false;
	}
	if (stSentParam.iUnsigned16 != stRecvParam.iUnsigned16)
	{
		EchoMgr::GetInstance().Note("u16");
		bSame = false;
	}
	if (stSentParam.iUnsigned32 != stRecvParam.iUnsigned32)
	{
		EchoMgr::GetInstance().Note("u32");
		bSame = false;
	}
	if (stSentParam.iUnsigned64 != stRecvParam.iUnsigned64)
	{
		EchoMgr::GetInstance().Note("u64");
		bSame = false;
	}
	if (stSentParam.vUnsignedNumbers != stRecvParam.vUnsignedNumbers)
	{
		EchoMgr::GetInstance().Note("unums");
		bSame = false;
	}
	if (stSentParam.mUnsignedTable != stRecvParam.mUnsignedTable)
	{
		EchoMgr::GetInstance().Note("utable");
		bSame = false;
	}
	if (stSentParam.mLargeUnsignedTable != stRecvParam.mLargeUnsignedTable)
	{
		EchoMgr::GetInstance().Note("ubig");
		bSame = false;
	}
	if (stSentParam.mKeyed.size() != stRecvParam.mKeyed.size())
	{
		EchoMgr::GetInstance().Note("keyed");
		bSame = false;
	}
	else
	{
		for (const auto& [stKey, sValue] : stSentParam.mKeyed)
		{
			auto stJt = stRecvParam.mKeyed.find(stKey);
			if (stJt == stRecvParam.mKeyed.end() || stJt->second != sValue)
			{
				EchoMgr::GetInstance().Note("keyed");
				bSame = false;
				break;
			}
		}
	}
	if (stSentParam.iGap != stRecvParam.iGap)
	{
		EchoMgr::GetInstance().Note("gap");
		EchoMgr::GetInstance().NoteGap();
		bSame = false;
	}
	return bSame;
}

static coro::Task RunOne(int iNumberParam)
{
	::OI::RpcData::AllEchoReq stRequestLocal;
	FillAllTypes(stRequestLocal.stData, iNumberParam);
	::OI::RpcData::AllEchoRes stResponseLocal;
	const int32_t iReturnCode = co_await IpcClient::GetInstance().Call(stRequestLocal, stResponseLocal);
	const bool bSame = iReturnCode == 0 && DiffAllTypes(stRequestLocal.stData, stResponseLocal.stData);
	EchoMgr::GetInstance().OnResult(iNumberParam, stRequestLocal, stResponseLocal, iReturnCode, bSame);
}

EchoMgr::EchoMgr()
    : m_bEnabled(true), m_bStack(false), m_iTotal(1000000), m_iInflight(2048), m_iTimeoutMilliseconds(180000), m_iNext(0), m_iSessionId(0),
      m_iDone(0), m_iOk(0), m_iRpcFail(0), m_iMismatch(0), m_iGapDiffer(0),
      m_iSampleSent(0), m_iSampleReceived(0), m_iSampleCode(0), m_bHaveSample(false)
{
}

void EchoMgr::Load()
{
	TryGetConfig("Test.echo", m_bEnabled);
	TryGetConfig("Test.stack", m_bStack);
	TryGetConfig("TestEcho.total", m_iTotal);
	TryGetConfig("TestEcho.inflight", m_iInflight);
	if (m_iInflight < 1)
	{
		m_iInflight = 1;
	}
	TryGetConfig("TestEcho.timeout_ms", m_iTimeoutMilliseconds);
}

void EchoMgr::Note(const char* pNameParam)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	m_mFields[pNameParam] += 1;
}

void EchoMgr::NoteGap()
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	++m_iGapDiffer;
}

void EchoMgr::ArmTimeout()
{
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([]()
	{
		LOG_TRACE("ipc timeout done=%d ok=%d", EchoMgr::GetInstance().Done(), EchoMgr::GetInstance().Ok());
		fflush(stdout);
		::OI::Thread::ThreadPool::GetInstance().RequestStop(1);
	}, m_iTimeoutMilliseconds);
}

static coro::TaskResult<bool> StackOne(int iNumberParam, std::shared_ptr<std::atomic<int32_t>> pDoneParam)
{
	::OI::RpcData::AllEchoReq stRequestLocal;
	FillAllTypes(stRequestLocal.stData, iNumberParam);
	auto stResult = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::AllEchoRes>(stRequestLocal, ::OI::Timer::Seconds(20));
	pDoneParam->fetch_add(1);
	co_return stResult.GetRet() == OI_OK&& stResult.GetRes().iCode == 1;
}

coro::Task EchoMgr::StackProbe()
{
	TestBoard::GetInstance().Arm();
	// 只用来打印中途进度；结果从任务本身取。
	auto pDone = std::make_shared<std::atomic<int32_t>>(0);
	std::vector<coro::TaskResult<bool>> vCalls;
	for (int iNumber = 0; iNumber < 3; ++iNumber)
	{
		vCalls.push_back(StackOne(iNumber, pDone));
	}
	co_await coro::yield(400);
	LOG_TRACE("stack holding done=%d", pDone->load());
	fflush(stdout);
	int iOkLocal = 0;
	for (const auto& stCall : vCalls)
	{
		iOkLocal += co_await stCall ? 1 : 0;
	}
	LOG_TRACE("stack flush ok=%d done=%d", iOkLocal, pDone->load());
	fflush(stdout);
	int iSpins = 0;
	while (IpcClient::GetInstance().IsConnect() && iSpins++ < 200)
	{
		co_await coro::yield(50);
	}
	co_await coro::yield(4000);
	::OI::RpcData::AllEchoReq stRequestLocal;
	FillAllTypes(stRequestLocal.stData, 9);
	auto stResult = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::AllEchoRes>(stRequestLocal, ::OI::Timer::Seconds(3));
	int iReturnCodeLocal = stResult.GetRet();
	LOG_TRACE("stack offline ret=%d", iReturnCodeLocal);
	fflush(stdout);
	TestBoard::GetInstance().Done((iOkLocal == 3 && iReturnCodeLocal == OI_ERR_CONNECT) ? 0 : 1);
	co_return;
}

void EchoMgr::Start(sid_t iSessionIdParam)
{
	if (!m_bEnabled || m_iTotal <= 0)
	{
		return;
	}
	m_iSessionId = iSessionIdParam;
	m_stStart = std::chrono::steady_clock::now();
	ArmTimeout();
	LOG_TRACE("ipc client connect sid=%d mode=%s total=%d", m_iSessionId, kMode, m_iTotal);
	fflush(stdout);
	TestBoard::GetInstance().Arm();
	coro::StartTask([](coro::Token)
	{
		return EchoMgr::GetInstance().Run();
	});
}

coro::Task EchoMgr::Run()
{
	m_iNext.store(0);
	// 在途数量等于工作协程数，不超过服务端每条会话的额度，不会被回 Busy。
	const int32_t iWorkers = m_iInflight < m_iTotal ? m_iInflight : m_iTotal;
	std::vector<coro::Task> vWorkers;
	for (int32_t iWorker = 0; iWorker < iWorkers; ++iWorker)
	{
		vWorkers.push_back(Pump());
	}
	co_await coro::WhenAll(std::move(vWorkers));
	Report();
}

coro::Task EchoMgr::Pump()
{
	for (;;)
	{
		const int32_t iNumber = m_iNext.fetch_add(1);
		if (iNumber >= m_iTotal)
		{
			co_return;
		}
		co_await RunOne(iNumber);
	}
}

void EchoMgr::OnResult(int iNumberParam, const ::OI::RpcData::AllEchoReq& stRequestParam, const ::OI::RpcData::AllEchoRes& stResponseParam, int iReturnCodeParam, bool bSameParam)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	if (!m_bHaveSample && iNumberParam == 0)
	{
		m_bHaveSample = true;
		m_iSampleSent = stRequestParam.stData.iGap;
		m_iSampleReceived = stResponseParam.stData.iGap;
		m_iSampleCode = stResponseParam.iCode;
	}
	if (iReturnCodeParam != 0 || stResponseParam.iCode != 1)
	{
		++m_mFields["ret_" + std::to_string(iReturnCodeParam)];
		m_iRpcFail.fetch_add(1);
	}
	else if (bSameParam)
	{
		m_iOk.fetch_add(1);
	}
	else
	{
		m_iMismatch += 1;
	}
	m_iDone.fetch_add(1);
}

void EchoMgr::Report()
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	long long iMillisecondsLocal = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - m_stStart)
	                                   .count();
	if (iMillisecondsLocal < 1)
	{
		iMillisecondsLocal = 1;
	}
	int iOkLocal = m_iOk.load();
	int iRpcFail = m_iRpcFail.load();
	LOG_TRACE("ipc mode=%s total=%d ok=%d mismatch=%d rpc_fail=%d ms=%lld rate=%d/s",
	    kMode, m_iTotal, iOkLocal, m_iMismatch, iRpcFail, iMillisecondsLocal, (int)(m_iTotal * 1000LL / iMillisecondsLocal));
	LOG_TRACE("sample gap_sent=%d gap_recv=%d code=%d gap_differ=%d",
	    m_iSampleSent, m_iSampleReceived, m_iSampleCode, m_iGapDiffer);
	std::string sFields = "fields";
	for (const auto& [sName, iCount] : m_mFields)
	{
		sFields += " " + sName + "=" + std::to_string(iCount);
	}
	LOG_TRACE("%s", sFields.c_str());
	fflush(stdout);
	TestBoard::GetInstance().Done((m_iMismatch == 0 && iRpcFail == 0 && iOkLocal == m_iTotal) ? 0 : 1);
}
