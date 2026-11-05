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

static bool SameBytes(const ::OI::Base::Bytes& stFirst, const ::OI::Base::Bytes& stSecond)
{
	if (stFirst.Size() != stSecond.Size())
	{
		return false;
	}
	if (stFirst.Size() == 0)
	{
		return true;
	}
	return memcmp(stFirst.Data(), stSecond.Data(), stFirst.Size()) == 0;
}

static bool SameInner(const ::OI::RpcData::Inner& stFirst, const ::OI::RpcData::Inner& stSecond)
{
	return stFirst.iNumber == stSecond.iNumber && stFirst.sText == stSecond.sText;
}

static void FillAllTypes(::OI::RpcData::AllTypesData& stData, int iNumberLocal)
{
	stData.iInt8 = (int8_t)(iNumberLocal & 0x7f);
	stData.iInt16 = (int16_t)(1000 + iNumberLocal);
	stData.iInt32 = iNumberLocal;
	stData.iInt64 = 100000LL + iNumberLocal;
	stData.iAlternateInt64 = 10000000000LL + iNumberLocal;
	stData.fFloat = 1.5f + (float)iNumberLocal;
	stData.dDouble = 2.5 + iNumberLocal;
	stData.stRawData.Clear();
	char vRaw[4];
	memcpy(vRaw, &iNumberLocal, 4);
	stData.stRawData.Append(vRaw, 4);
	stData.sMessage = "msg-" + std::to_string(iNumberLocal);
	stData.vNumbers.clear();
	stData.vNumbers.push_back(iNumberLocal);
	stData.vNumbers.push_back(iNumberLocal + 1);
	stData.vWords.clear();
	stData.vWords.push_back("w");
	stData.vWords.push_back(std::to_string(iNumberLocal));
	stData.stChild.iNumber = iNumberLocal;
	stData.stChild.sText = "child";
	stData.vChildren.clear();
	stData.vChildren.push_back(stData.stChild);
	::OI::RpcData::Inner stKin;
	stKin.iNumber = iNumberLocal + 7;
	stKin.sText = "kin";
	stData.vChildren.push_back(stKin);
	stData.mTable.clear();
	stData.mTable[iNumberLocal] = "t";
	stData.mTable[iNumberLocal + 1] = "u";
	stData.mNamed.clear();
	stData.mNamed["a"] = stData.stChild;
	stData.mNamed["b"] = stKin;
	stData.iUnsigned8 = (uint8_t)200;
	stData.iUnsigned16 = (uint16_t)40000;
	stData.iUnsigned32 = 3000000000u + (uint32_t)(iNumberLocal & 0xff);
	stData.iUnsigned64 = (1ULL << 63) + (uint64_t)iNumberLocal;
	stData.vUnsignedNumbers.clear();
	stData.vUnsignedNumbers.push_back(stData.iUnsigned64);
	stData.vUnsignedNumbers.push_back(1);
	stData.iGap = 20 + iNumberLocal;
	stData.mUnsignedTable.clear();
	stData.mUnsignedTable[stData.iUnsigned32] = "u";
	stData.mLargeUnsignedTable.clear();
	stData.mLargeUnsignedTable[stData.iUnsigned64] = "b";
	stData.mKeyed.clear();
	::OI::RpcData::EchoKey stKey;
	stKey.stPart.iNumber = iNumberLocal;
	stKey.sName = "k";
	stData.mKeyed[stKey] = "v";
	stKey.stPart.iNumber = iNumberLocal + 1;
	stKey.sName = "k2";
	stData.mKeyed[stKey] = "w";
}

static void PoisonAllTypes(::OI::RpcData::AllTypesData& stData)
{
	stData.iInt8 = 0x5a;
	stData.iInt16 = -1;
	stData.iInt32 = -2;
	stData.iInt64 = -3;
	stData.iAlternateInt64 = -4;
	stData.fFloat = -5.0f;
	stData.dDouble = -6.0;
	stData.stRawData.Clear();
	stData.sMessage = "poison";
	stData.vNumbers.clear();
	stData.vWords.clear();
	stData.stChild.iNumber = -7;
	stData.stChild.sText = "poison";
	stData.vChildren.clear();
	stData.mTable.clear();
	stData.mNamed.clear();
	stData.iUnsigned8 = 1;
	stData.iUnsigned16 = 1;
	stData.iUnsigned32 = 1;
	stData.iUnsigned64 = 1;
	stData.vUnsignedNumbers.clear();
	stData.iGap = -12345;
	stData.mUnsignedTable.clear();
	stData.mLargeUnsignedTable.clear();
	stData.mKeyed.clear();
}

static bool DiffAllTypes(const ::OI::RpcData::AllTypesData& stSent, const ::OI::RpcData::AllTypesData& stRecv)
{
	bool bSame = true;
	if (stSent.iInt8 != stRecv.iInt8)
	{
		EchoMgr::GetInstance().Note("c");
		bSame = false;
	}
	if (stSent.iInt16 != stRecv.iInt16)
	{
		EchoMgr::GetInstance().Note("s");
		bSame = false;
	}
	if (stSent.iInt32 != stRecv.iInt32)
	{
		EchoMgr::GetInstance().Note("i");
		bSame = false;
	}
	if (stSent.iInt64 != stRecv.iInt64)
	{
		EchoMgr::GetInstance().Note("l");
		bSame = false;
	}
	if (stSent.iAlternateInt64 != stRecv.iAlternateInt64)
	{
		EchoMgr::GetInstance().Note("i64");
		bSame = false;
	}
	if (stSent.fFloat != stRecv.fFloat)
	{
		EchoMgr::GetInstance().Note("f");
		bSame = false;
	}
	if (stSent.dDouble != stRecv.dDouble)
	{
		EchoMgr::GetInstance().Note("d");
		bSame = false;
	}
	if (!SameBytes(stSent.stRawData, stRecv.stRawData))
	{
		EchoMgr::GetInstance().Note("raw");
		bSame = false;
	}
	if (stSent.sMessage != stRecv.sMessage)
	{
		EchoMgr::GetInstance().Note("msg");
		bSame = false;
	}
	if (stSent.vNumbers != stRecv.vNumbers)
	{
		EchoMgr::GetInstance().Note("nums");
		bSame = false;
	}
	if (stSent.vWords != stRecv.vWords)
	{
		EchoMgr::GetInstance().Note("words");
		bSame = false;
	}
	if (!SameInner(stSent.stChild, stRecv.stChild))
	{
		EchoMgr::GetInstance().Note("child");
		bSame = false;
	}
	if (stSent.vChildren.size() != stRecv.vChildren.size())
	{
		EchoMgr::GetInstance().Note("children");
		bSame = false;
	}
	else
	{
		for (size_t iIndexLocal = 0; iIndexLocal < stSent.vChildren.size(); ++iIndexLocal)
		{
			if (!SameInner(stSent.vChildren[iIndexLocal], stRecv.vChildren[iIndexLocal]))
			{
				EchoMgr::GetInstance().Note("children");
				bSame = false;
				break;
			}
		}
	}
	if (stSent.mTable != stRecv.mTable)
	{
		EchoMgr::GetInstance().Note("table");
		bSame = false;
	}
	if (stSent.mNamed.size() != stRecv.mNamed.size())
	{
		EchoMgr::GetInstance().Note("named");
		bSame = false;
	}
	else
	{
		for (const auto& [sName, stValue] : stSent.mNamed)
		{
			auto stJt = stRecv.mNamed.find(sName);
			if (stJt == stRecv.mNamed.end() || !SameInner(stValue, stJt->second))
			{
				EchoMgr::GetInstance().Note("named");
				bSame = false;
				break;
			}
		}
	}
	if (stSent.iUnsigned8 != stRecv.iUnsigned8)
	{
		EchoMgr::GetInstance().Note("u8");
		bSame = false;
	}
	if (stSent.iUnsigned16 != stRecv.iUnsigned16)
	{
		EchoMgr::GetInstance().Note("u16");
		bSame = false;
	}
	if (stSent.iUnsigned32 != stRecv.iUnsigned32)
	{
		EchoMgr::GetInstance().Note("u32");
		bSame = false;
	}
	if (stSent.iUnsigned64 != stRecv.iUnsigned64)
	{
		EchoMgr::GetInstance().Note("u64");
		bSame = false;
	}
	if (stSent.vUnsignedNumbers != stRecv.vUnsignedNumbers)
	{
		EchoMgr::GetInstance().Note("unums");
		bSame = false;
	}
	if (stSent.mUnsignedTable != stRecv.mUnsignedTable)
	{
		EchoMgr::GetInstance().Note("utable");
		bSame = false;
	}
	if (stSent.mLargeUnsignedTable != stRecv.mLargeUnsignedTable)
	{
		EchoMgr::GetInstance().Note("ubig");
		bSame = false;
	}
	if (stSent.mKeyed.size() != stRecv.mKeyed.size())
	{
		EchoMgr::GetInstance().Note("keyed");
		bSame = false;
	}
	else
	{
		for (const auto& [stKey, sValue] : stSent.mKeyed)
		{
			auto stJt = stRecv.mKeyed.find(stKey);
			if (stJt == stRecv.mKeyed.end() || stJt->second != sValue)
			{
				EchoMgr::GetInstance().Note("keyed");
				bSame = false;
				break;
			}
		}
	}
	if (stSent.iGap != stRecv.iGap)
	{
		EchoMgr::GetInstance().Note("gap");
		EchoMgr::GetInstance().NoteGap();
		bSame = false;
	}
	return bSame;
}

static OI::Coro::Task RunOne(int iNumberLocal)
{
	::OI::RpcData::AllEchoReq stRequestLocal;
	FillAllTypes(stRequestLocal.stData, iNumberLocal);
	::OI::RpcData::AllEchoRes stResponseLocal;
	const int32_t iReturnCode = co_await IpcClient::GetInstance().Call(stRequestLocal, stResponseLocal);
	const bool bSame = iReturnCode == 0 && DiffAllTypes(stRequestLocal.stData, stResponseLocal.stData);
	EchoMgr::GetInstance().OnResult(iNumberLocal, stRequestLocal, stResponseLocal, iReturnCode, bSame);
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

void EchoMgr::Note(const char* pName)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	m_mFields[pName] += 1;
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

static OI::Coro::TaskResult<bool> StackOne(int iNumberLocal, std::shared_ptr<std::atomic<int32_t>> pDone)
{
	::OI::RpcData::AllEchoReq stRequestLocal;
	FillAllTypes(stRequestLocal.stData, iNumberLocal);
	auto stResult = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::AllEchoRes>(stRequestLocal, ::OI::Timer::Seconds(20));
	pDone->fetch_add(1);
	co_return stResult.GetRet() == OI_OK&& stResult.GetRes().iCode == 1;
}

OI::Coro::Task EchoMgr::StackProbe()
{
	TestBoard::GetInstance().Arm();
	// 只用来打印中途进度；结果从任务本身取。
	auto pDone = std::make_shared<std::atomic<int32_t>>(0);
	std::vector<OI::Coro::TaskResult<bool>> vCalls;
	for (int iNumber = 0; iNumber < 3; ++iNumber)
	{
		vCalls.push_back(StackOne(iNumber, pDone));
	}
	co_await OI::Coro::yield(400);
	LOG_TRACE("stack holding done=%d", pDone->load());
	fflush(stdout);
	const auto vOk = co_await OI::Coro::WhenAll(std::move(vCalls));
	int iOkLocal = 0;
	for (const auto iOk : vOk)
	{
		iOkLocal += iOk;
	}
	LOG_TRACE("stack flush ok=%d done=%d", iOkLocal, pDone->load());
	fflush(stdout);
	int iSpins = 0;
	while (IpcClient::GetInstance().IsConnect() && iSpins++ < 200)
	{
		co_await OI::Coro::yield(50);
	}
	co_await OI::Coro::yield(4000);
	::OI::RpcData::AllEchoReq stRequestLocal;
	FillAllTypes(stRequestLocal.stData, 9);
	auto stResult = co_await IpcClient::GetInstance().CallAny<::OI::RpcData::AllEchoRes>(stRequestLocal, ::OI::Timer::Seconds(3));
	int iReturnCodeLocal = stResult.GetRet();
	LOG_TRACE("stack offline ret=%d", iReturnCodeLocal);
	fflush(stdout);
	TestBoard::GetInstance().Done((iOkLocal == 3 && iReturnCodeLocal == OI_ERR_CONNECT) ? 0 : 1);
	co_return;
}

void EchoMgr::Start(sid_t iSessionId)
{
	if (!m_bEnabled || m_iTotal <= 0)
	{
		return;
	}
	m_iSessionId = iSessionId;
	m_stStart = std::chrono::steady_clock::now();
	ArmTimeout();
	LOG_TRACE("ipc client connect sid=%d mode=%s total=%d", m_iSessionId, kMode, m_iTotal);
	fflush(stdout);
	TestBoard::GetInstance().Arm();
	OI::Coro::StartTask([](OI::Coro::Token)
	{
		return EchoMgr::GetInstance().Run();
	});
}

OI::Coro::Task EchoMgr::Run()
{
	m_iNext.store(0);
	// 在途数量等于工作协程数，不超过服务端每条会话的额度，不会被回 Busy。
	const int32_t iWorkers = m_iInflight < m_iTotal ? m_iInflight : m_iTotal;
	std::vector<OI::Coro::Task> vWorkers;
	for (int32_t iWorker = 0; iWorker < iWorkers; ++iWorker)
	{
		vWorkers.push_back(Pump());
	}
	co_await OI::Coro::WhenAll(std::move(vWorkers));
	Report();
}

OI::Coro::Task EchoMgr::Pump()
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

void EchoMgr::OnResult(int iNumberLocal, const ::OI::RpcData::AllEchoReq& stRequest, const ::OI::RpcData::AllEchoRes& stResponse, int iReturnCode, bool bSame)
{
	::OI::Thread::MutexGuard stLock(m_stMutex);
	if (!m_bHaveSample && iNumberLocal == 0)
	{
		m_bHaveSample = true;
		m_iSampleSent = stRequest.stData.iGap;
		m_iSampleReceived = stResponse.stData.iGap;
		m_iSampleCode = stResponse.iCode;
	}
	if (iReturnCode != 0 || stResponse.iCode != 1)
	{
		++m_mFields["ret_" + std::to_string(iReturnCode)];
		m_iRpcFail.fetch_add(1);
	}
	else if (bSame)
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
