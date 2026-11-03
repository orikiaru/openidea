#pragma once
#include "oi_rpcdata_allechoreq.h"
#include "oi_rpcdata_allechores.h"
#include "oi_rpc_coro.h"
#include "oi_singleton.h"
#include <atomic>
#include <chrono>
#include <map>
#include "oi_lock.h"
#include <string>

class EchoMgr
{
	DECLARE_SINGLETON(EchoMgr)
	EchoMgr();
	bool m_bEnabled;
	bool m_bStack;
	int32_t m_iTotal;
	int32_t m_iTimeoutMilliseconds;
	sid_t m_iSessionId;
	std::chrono::steady_clock::time_point m_stStart;
	std::atomic<int32_t> m_iDone;
	std::atomic<int32_t> m_iOk;
	std::atomic<int32_t> m_iRpcFail;
	::OI::Thread::Mutex m_stMutex;
	int32_t m_iMismatch;
	int32_t m_iGapDiffer;
	int32_t m_iSampleSent;
	int32_t m_iSampleReceived;
	int32_t m_iSampleCode;
	bool m_bHaveSample;
	std::map<std::string, int32_t> m_mFields;
public:
	void Load();
	bool StackOn() const { return m_bStack; }
	coro::Task StackProbe();
	void ArmTimeout();
	void Start(sid_t iSessionIdParam);
	sid_t Sid() const { return m_iSessionId; }
	int32_t Done() const { return m_iDone.load(); }
	int32_t Ok() const { return m_iOk.load(); }
	void Note(const char* pNameParam);
	void NoteGap();
	void OnResult(int iNumberParam, const ::OI::RpcData::AllEchoReq& stRequestParam, const ::OI::RpcData::AllEchoRes& stResponseParam, int iReturnCodeParam, bool bSameParam);
	coro::Task Pump(int iIndexParam);
};
