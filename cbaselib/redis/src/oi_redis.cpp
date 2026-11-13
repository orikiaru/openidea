#include "oi_redis.h"
#include "oi_redis_session.h"
#include "oi_rpc_coro.h"
#include "oi_poll.h"
#include "oi_timer_manager.h"
#include "oi_config.h"
#include "oi_log.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <string_view>
#include <uv.h>
namespace OI
{
namespace Redis
{

int32_t RedisResult::GetRet() const noexcept
{
	switch (iStatus)
	{
	case RedisStatus::WrongThread:
		return OI_ERR_WRONG_THREAD;
	case RedisStatus::Ok:
		return pReply ? OI_OK : OI_ERR_PROTOCOL;
	case RedisStatus::Timeout:
		return OI_ERR_TIMEOUT;
	case RedisStatus::Disconnected:
		return OI_ERR_CONNECT;
	case RedisStatus::InvalidArgument:
		return OI_ERR_INVALID_ARGUMENT;
	case RedisStatus::SendFailed:
		return OI_ERR_FAIL;
	case RedisStatus::Busy:
		return OI_ERR_BUSY;
	case RedisStatus::ProtocolError:
		return OI_ERR_PROTOCOL;
	case RedisStatus::ServerError:
		if (pReply && pReply->str && pReply->len >= 10 && std::string_view(pReply->str, 10) == "WRONGTYPE ")
		{
			return OI_ERR_REDIS_WRONGTYPE;
		}
		return OI_ERR_REDIS;
	}
	return OI_ERR_PROTOCOL;
}

namespace
{
void ReportRedisError(uint64_t iRequestId, sid_t iSessionIdLocal, const char* pStage,
    std::string_view sCommand, std::string_view sReason, bool bLate = false) noexcept
{
	const ::OI::Log::DiagnosticText stCommand(sCommand.substr(0, 64));
	const ::OI::Log::DiagnosticText stReason(sReason);
	try
	{
		LOG_ERROR("redis failure stage=%s request=%llu sid=%llu command=%s reason=%s truncated=%d late=%d",
		    pStage, (unsigned long long)iRequestId, (unsigned long long)iSessionIdLocal,
		    stCommand.GetText(), stReason.GetText(), stReason.IsTruncated() ? 1 : 0, bLate ? 1 : 0);
		// 错误立即刷新本地输出，避免日志服不可用时仍滞留在 stdout 缓冲。
		fflush(stdout);
	}
	catch (...)
	{
		fprintf(stderr, "redis failure stage=%s request=%llu sid=%llu command=%s reason=%s truncated=%d late=%d logger_failed=1\n",
		    pStage, (unsigned long long)iRequestId, (unsigned long long)iSessionIdLocal,
		    stCommand.GetText(), stReason.GetText(), stReason.IsTruncated() ? 1 : 0, bLate ? 1 : 0);
	}
}

struct ReplyFree
{
	void operator()(redisReply* pReplyLocal) const
	{
		freeReplyObject(pReplyLocal);
	}
};

uv_timer_t g_stSweepTimer;
bool g_bSweepInitialized = false; // 仅 poll 线程访问。
const int kSweepMs = 200;

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now().time_since_epoch())
	    .count();
}

bool ArmRedisSweep()
{
	if (!g_bSweepInitialized)
	{
		if (uv_timer_init(::OI::Net::Poll::GetInstance().Loop(), &g_stSweepTimer) != 0)
		{
			return false;
		}
		g_bSweepInitialized = true;
	}
	if (uv_is_active(reinterpret_cast<uv_handle_t*>(&g_stSweepTimer)))
	{
		return true;
	}
	// 超时直接由 poll 推进，不能依赖可能正在 CallSync 的工人线程。
	return uv_timer_start(&g_stSweepTimer, [](uv_timer_t*)
	{
		RedisClient::SweepTimeout(nullptr);
	}, kSweepMs, kSweepMs) == 0;
}
}

struct RedisCall::State
{
	uint64_t iRequestId = OI::Coro::AllocTaskUid(); // 这一次调用的配对号。旧会话的回包对不上就丢掉。
	std::vector<std::string> vArguments;            // 命令和参数。
	std::shared_ptr<redisReply> pReply;             // 回包。exchange(Ready) 之前写完。
	RedisStatus iStatus;                            // 结果状态。和 pReply 一起在认领成功后写。
	bool bReady;                                    // 同步等待看它。协程等待看 iGate。
	bool bSynchronous;                              // 真则用条件变量，不进完成门。
	RedisResult* pOutput;                           // 调用方给出的输出。resume 时再填。
	std::coroutine_handle<> stHandle;               // 调用方句柄。门已经是 Waiting 才 Schedule。
	int64_t iDeadlineMilliseconds;                  // 绝对截止。
	std::atomic<OI::Coro::Gate> iGate;              // 完成门。Idle 还没人认领，Ready 结果已写好，Waiting 调用方已挂起。
	std::atomic<int32_t> iWon;                      // 回包、超时、断连只有一个能写下结果。
	std::atomic<int32_t> iReferences;               // 调用方和超时扫描各持一份。
	std::atomic<bool> bAwaitStarted{false};         // 一次性闩。第二次 await 或移走后再等直接抛。
	sid_t iSessionId;                               // 发出时绑定的会话。
	::OI::Thread::Mutex stMutex;
	::OI::Thread::MutexCond stConditionVariable;
	State()
	    : iStatus(RedisStatus::Disconnected), bReady(false), bSynchronous(false), pOutput(NULL), iDeadlineMilliseconds(0), iGate(OI::Coro::Gate::Idle), iWon(0), iReferences(1), iSessionId(INVALID_SID)
	{
	}
};

RedisClient::RedisClient()
    : m_pReader(NULL), m_iTimeoutMilliseconds(25000), m_iMaxPending(65536), m_iSwept(0)
{
}

RedisClient::~RedisClient()
{
	if (m_pReader)
	{
		redisReaderFree(m_pReader);
		m_pReader = NULL;
	}
}

int32_t RedisClient::Init()
{
	const int32_t iThreadStatus = ::OI::Net::Poll::GetInstance().CheckThreadAccess("RedisClient::Init");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	TryGetConfig("RedisClient.timeout_ms", m_iTimeoutMilliseconds);
	TryGetConfig("RedisClient.max_pending", m_iMaxPending);
	if (m_iMaxPending < 1)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (m_iTimeoutMilliseconds <= 0)
	{
		m_iTimeoutMilliseconds = 25000;
	}
	int32_t iCode = ::OI::Rpc::ProtoClientManager::Init();
	if (iCode != OI_OK)
	{
		return iCode;
	}
	return ::OI::Thread::ThreadPool::GetInstance().AddStopHandler([]()
	{
		auto& stClient = GetInstance();
		stClient.FailAll();
		if (g_bSweepInitialized)
		{
			uv_timer_stop(&g_stSweepTimer);
		}
		stClient.m_iReaderSessionId = INVALID_SID;
		stClient.CloseAllSessions();
	});
}

::OI::Net::Session* RedisClient::CloneSession()
{
	return new RedisClientSession(this);
}

void RedisClient::OnClientConnect(::OI::Net::Session* pSession)
{
	// 新连接发布前先结束旧等待；旧连接的后续回包由 Feed 的 sid 检查丢弃。
	FailAll();
	m_iReaderSessionId = pSession->GetSessionID();
	if (m_pReader)
	{
		redisReaderFree(m_pReader);
	}
	m_pReader = redisReaderCreate();
	LOG_TRACE("RedisClient::OnClientConnect sid=%d", m_iReaderSessionId);
}

void RedisClient::OnClientDisconnect(const ::OI::Net::Session* pSession)
{
	sid_t iSessionIdLocal = pSession->GetSessionID();
	LOG_TRACE("RedisClient::OnClientDisconnect sid=%d", iSessionIdLocal);
	if (m_iReaderSessionId == iSessionIdLocal)
	{
		m_iReaderSessionId = INVALID_SID;
		if (m_pReader)
		{
			redisReaderFree(m_pReader);
			m_pReader = NULL;
		}
	}
	FailSid(iSessionIdLocal);
}

void RedisClient::Release(RedisCall::State* pSt)
{
	if (!pSt)
	{
		return;
	}
	if (pSt->iReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete pSt;
	}
}

RedisCall::RedisCall(std::vector<std::string> vArgumentsLocal, RedisResult* pOutputLocal)
    : m_pState(new State())
{
	m_pState->vArguments = std::move(vArgumentsLocal);
	m_pState->pOutput = pOutputLocal;
}

RedisCall::RedisCall(RedisCall&& stOther) noexcept
    : m_pState(stOther.m_pState)
{
	stOther.m_pState = NULL;
}

RedisCall::~RedisCall()
{
	RedisClient::Release(m_pState);
}

RedisResult RedisCall::await_resume()
{
	RedisResult stResultLocal;
	stResultLocal.pReply = m_pState->pReply;
	stResultLocal.iStatus = m_pState->iStatus;
	stResultLocal.bTimeout = stResultLocal.iStatus == RedisStatus::Timeout;
	if (m_pState->pOutput)
	{
		*m_pState->pOutput = stResultLocal;
	}
	return stResultLocal;
}

bool RedisCall::Suspend(std::coroutine_handle<> stHandleLocal)
{
	// 在写入等待句柄、检查停止或提交命令前认领；失败也不能重放写操作。
	if (!m_pState || m_pState->bAwaitStarted.exchange(true, std::memory_order_acq_rel))
	{
		throw std::logic_error("RedisCall can only be awaited once and must not be moved from");
	}
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("RedisCall::Suspend") != OI_OK)
	{
		m_pState->iStatus = RedisStatus::WrongThread;
		return false;
	}

	if (::OI::Thread::ThreadPool::GetInstance().StopRequested())
	{
		return false;
	}
	m_pState->stHandle = stHandleLocal;
	::OI::Net::Poll::GetInstance().CallOnLoop(RedisClient::Submit, m_pState);
	OI::Coro::Gate iPrevious = m_pState->iGate.exchange(OI::Coro::Gate::Waiting, std::memory_order_acq_rel);
	return iPrevious != OI::Coro::Gate::Ready;
}

RedisCall RedisClient::Call(std::vector<std::string> vArgumentsLocal, RedisResult* pOutputLocal)
{
	return RedisCall(std::move(vArgumentsLocal), pOutputLocal);
}

RedisResult RedisClient::CallSync(std::vector<std::string> vArgumentsLocal)
{
	if (::OI::Net::Poll::GetInstance().CheckThreadAccess("RedisClient::CallSync") != OI_OK)
	{
		RedisResult stRejected;
		stRejected.iStatus = RedisStatus::WrongThread;
		return stRejected;
	}

	// 两种模式都禁止在 libuv 回调中同步等待，不能重入事件循环。
	if (::OI::Net::Poll::GetInstance().InPollCallback())
	{
		throw std::logic_error("CallRedisSync cannot run in a poll callback; use co_await CallRedis");
	}
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested())
	{
		return {};
	}
	RedisCall::State* pSt = new RedisCall::State();
	pSt->vArguments = std::move(vArgumentsLocal);
	pSt->bSynchronous = true;
	::OI::Net::Poll::GetInstance().CallOnLoop(Submit, pSt);
#ifdef _SINGLE_THREAD_
	while (!pSt->bReady && !::OI::Thread::ThreadPool::GetInstance().Stopping())
	{
		// 每轮先自己扫一次超时，不等扫描定时器的下一跳。
		SweepTimeout(nullptr);
		if (!pSt->bReady)
		{
			::OI::Thread::ThreadPool::GetInstance().PumpOnce();
		}
	}
	if (!pSt->bReady)
	{
		Settle(pSt, {}, RedisStatus::Disconnected);
	}
#else
	{
		::OI::Thread::MutexGuard stLock(pSt->stMutex);
		while (!pSt->bReady)
		{
			pSt->stConditionVariable.Wait(pSt->stMutex);
		}
	}
#endif
	RedisResult stOutput;
	stOutput.pReply = pSt->pReply;
	stOutput.iStatus = pSt->iStatus;
	stOutput.bTimeout = stOutput.iStatus == RedisStatus::Timeout;
	Release(pSt);
	return stOutput;
}

void RedisClient::Submit(void* pStateLocal)
{
	auto pState = static_cast<RedisCall::State*>(pStateLocal);
	// 排队期间由调用方保活；回调内立即完成可能唤醒并释放调用方，另持引用到返回。
	pState->iReferences.fetch_add(1, std::memory_order_relaxed);
	std::unique_ptr<RedisCall::State, decltype(&Release)> pHeld(pState, &Release);
	GetInstance().Enqueue(pState);
}

void RedisClient::SweepTimeout(void*)
{
	RedisClient& stSelf = GetInstance();
	int64_t iNow = NowMs();
	std::vector<RedisCall::State*> vExpired;
	{
		// 超时时长固定，又按发送顺序入队，所以 _wait 按截止时间有序。
		// 超时的仍留在队里认领它那条回执，_swept 记着已经判过的前缀，不重复扫。
		::OI::Thread::MutexGuard stLock(stSelf.m_stWaitMutex);
		while (stSelf.m_iSwept < stSelf.m_vWait.size())
		{
			RedisCall::State* pSt = stSelf.m_vWait[stSelf.m_iSwept];
			if (pSt->iDeadlineMilliseconds > iNow)
			{
				break;
			}
			vExpired.push_back(pSt);
			++stSelf.m_iSwept;
		}
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vExpired.size(); ++iIndexLocal)
	{
		stSelf.Settle(vExpired[iIndexLocal], std::shared_ptr<redisReply>(), RedisStatus::Timeout);
	}
}

void RedisClient::Settle(RedisCall::State* pSt, std::shared_ptr<redisReply> pReplyLocal, RedisStatus iStatusLocal)
{
	int iExpect = 0;
	if (!pSt->iWon.compare_exchange_strong(iExpect, 1, std::memory_order_acq_rel))
	{
		return;
	}
	const bool bSync = pSt->bSynchronous;
	{
		::OI::Thread::MutexGuard stLock(pSt->stMutex);
		pSt->pReply = std::move(pReplyLocal);
		pSt->iStatus = iStatusLocal;
		pSt->bReady = true;
		if (bSync)
		{
			pSt->stConditionVariable.Signal();
		}
	}
	// 同步调用可能在通知后立即释放状态，出锁后不再解引用它。
	if (bSync)
	{
		return;
	}
	OI::Coro::Gate iPrevious = pSt->iGate.exchange(OI::Coro::Gate::Ready, std::memory_order_acq_rel);
	if (iPrevious == OI::Coro::Gate::Waiting && pSt->stHandle)
	{
		OI::Coro::Schedule(pSt->stHandle);
	}
}

void RedisClient::Enqueue(RedisCall::State* pSt)
{
	if (pSt->vArguments.empty())
	{
		Settle(pSt, {}, RedisStatus::InvalidArgument);
		return;
	}
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested() || m_iReaderSessionId == INVALID_SID || ConnectedSid() != m_iReaderSessionId)
	{
		Settle(pSt, {}, RedisStatus::Disconnected);
		return;
	}
	std::vector<const char*> vArgumentsLocal(pSt->vArguments.size());
	std::vector<size_t> vArglen(pSt->vArguments.size());
	for (size_t iIndexLocal = 0; iIndexLocal < pSt->vArguments.size(); ++iIndexLocal)
	{
		vArgumentsLocal[iIndexLocal] = pSt->vArguments[iIndexLocal].c_str();
		vArglen[iIndexLocal] = pSt->vArguments[iIndexLocal].size();
	}
	char* pCmd = NULL;
	long long iNumber = redisFormatCommandArgv(&pCmd, (int)vArgumentsLocal.size(), vArgumentsLocal.data(), vArglen.data());
	if (iNumber < 0 || !pCmd)
	{
		Settle(pSt, {}, RedisStatus::InvalidArgument);
		return;
	}
	::OI::Base::Bytes stFrame;
	stFrame.Append(pCmd, (size_t)iNumber);
	redisFreeCommand(pCmd);
	if (!ArmRedisSweep())
	{
		Settle(pSt, {}, RedisStatus::SendFailed);
		return;
	}
	pSt->iDeadlineMilliseconds = NowMs() + m_iTimeoutMilliseconds;
	bool bQueued = false;
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		// 超时的也占着位置。满了就不再排。
		if (m_vWait.size() < static_cast<size_t>(m_iMaxPending))
		{
			pSt->iSessionId = m_iReaderSessionId;
			pSt->iReferences.fetch_add(1, std::memory_order_relaxed);
			m_vWait.push_back(pSt);
			bQueued = true;
		}
	}
	if (!bQueued)
	{
		Settle(pSt, {}, RedisStatus::Busy);
		return;
	}
	const int32_t iSendRet = SendBytesResult(pSt->iSessionId, stFrame);
	if (iSendRet != OI_OK)
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		for (size_t iIndexLocal = 0; iIndexLocal < m_vWait.size(); ++iIndexLocal)
		{
			if (m_vWait[iIndexLocal] == pSt)
			{
				m_vWait.erase(m_vWait.begin() + (std::ptrdiff_t)iIndexLocal);
				if (iIndexLocal < m_iSwept)
				{
					--m_iSwept;
				}
				break;
			}
		}
		const RedisStatus iStatus = iSendRet == OI_ERR_BUSY ? RedisStatus::Busy : (iSendRet == OI_ERR_CONNECT ? RedisStatus::Disconnected : RedisStatus::SendFailed);
		char vReason[160];
		snprintf(vReason, sizeof(vReason), "ret=%d bytes=%zu cause=%s", iSendRet, stFrame.Size(),
		    iSendRet == OI_ERR_BUSY ? "send queue capacity exceeded" : (iSendRet == OI_ERR_CONNECT ? "session disconnected" : "send failed"));
		ReportRedisError(pSt->iRequestId, pSt->iSessionId, "send", pSt->vArguments.front(), vReason);
		Settle(pSt, {}, iStatus);
		Release(pSt);
		return;
	}
}

void RedisClient::FailSid(sid_t iSessionIdLocal)
{
	std::deque<RedisCall::State*> vLeft;
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		std::deque<RedisCall::State*> vKeep;
		size_t iSwept = 0;
		for (size_t iIndexLocal = 0; iIndexLocal < m_vWait.size(); ++iIndexLocal)
		{
			if (m_vWait[iIndexLocal]->iSessionId == iSessionIdLocal)
			{
				vLeft.push_back(m_vWait[iIndexLocal]);
				continue;
			}
			if (iIndexLocal < m_iSwept)
			{
				++iSwept;
			}
			vKeep.push_back(m_vWait[iIndexLocal]);
		}
		m_vWait.swap(vKeep);
		m_iSwept = iSwept;
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vLeft.size(); ++iIndexLocal)
	{
		if (vLeft[iIndexLocal]->iWon.load(std::memory_order_acquire) == 0)
		{
			Settle(vLeft[iIndexLocal], {}, RedisStatus::Disconnected);
		}
		Release(vLeft[iIndexLocal]);
	}
}

void RedisClient::FailAll(RedisStatus iStatusLocal)
{
	std::deque<RedisCall::State*> vLeft;
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		vLeft.swap(m_vWait);
		m_iSwept = 0;
	}
	for (size_t iIndexLocal = 0; iIndexLocal < vLeft.size(); ++iIndexLocal)
	{
		if (vLeft[iIndexLocal]->iWon.load(std::memory_order_acquire) == 0)
		{
			Settle(vLeft[iIndexLocal], {}, iStatusLocal);
		}
		Release(vLeft[iIndexLocal]);
	}
}

void RedisClient::Feed(sid_t iSessionIdLocal, const char* pData, size_t iSize)
{
	if (iSessionIdLocal != m_iReaderSessionId || iSessionIdLocal == INVALID_SID)
	{
		return;
	}
	if (!m_pReader)
	{
		m_pReader = redisReaderCreate();
	}
	if (redisReaderFeed(m_pReader, pData, iSize) != REDIS_OK)
	{
		ReportRedisError(0, iSessionIdLocal, "reader_feed", "", m_pReader->errstr);
		FailAll(RedisStatus::ProtocolError);
		if (m_iReaderSessionId != INVALID_SID)
		{
			CloseSession(m_iReaderSessionId);
		}
		return;
	}
	for (;;)
	{
		void* pRaw = NULL;
		if (redisReaderGetReply(m_pReader, &pRaw) != REDIS_OK)
		{
			ReportRedisError(0, iSessionIdLocal, "reader_reply", "", m_pReader->errstr);
			FailAll(RedisStatus::ProtocolError);
			if (m_iReaderSessionId != INVALID_SID)
			{
				CloseSession(m_iReaderSessionId);
			}
			return;
		}
		if (!pRaw)
		{
			return;
		}
		std::shared_ptr<redisReply> pHeld((redisReply*)pRaw, ReplyFree());
		RedisCall::State* pSt = NULL;
		{
			::OI::Thread::MutexGuard stLock(m_stWaitMutex);
			if (m_vWait.empty())
			{
				continue;
			}
			pSt = m_vWait.front();
			m_vWait.pop_front();
			if (m_iSwept > 0)
			{
				--m_iSwept;
			}
		}
		// 即使调用已经超时，也保留迟到的 Redis 原始错误，不能因不再唤醒而吞掉诊断。
		if (pHeld->type == REDIS_REPLY_ERROR)
		{
			ReportRedisError(pSt->iRequestId, iSessionIdLocal, "server_reply",
			    pSt->vArguments.empty() ? std::string_view() : std::string_view(pSt->vArguments.front()),
			    pHeld->str ? std::string_view(pHeld->str, pHeld->len) : std::string_view("missing error text"),
			    pSt->iWon.load(std::memory_order_acquire) != 0);
		}
		if (pSt->iWon.load(std::memory_order_acquire) == 0)
		{
			const auto iStatusLocal = pHeld->type == REDIS_REPLY_ERROR ? RedisStatus::ServerError : RedisStatus::Ok;
			Settle(pSt, std::move(pHeld), iStatusLocal);
		}
		Release(pSt);
	}
}
}
}
