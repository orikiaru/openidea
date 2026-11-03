#include "oi_redis.h"
#include "oi_redis_session.h"
#include "oi_rpc_coro.h"
#include "oi_poll.h"
#include "oi_timer_manager.h"
#include "oi_config.h"
#include "oi_log.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>
namespace OI { namespace Redis {

namespace
{
struct ReplyFree
{
	void operator()(redisReply* pReplyParam) const
	{
		freeReplyObject(pReplyParam);
	}
};

std::once_flag g_sweep_once;
const int kSweepMs = 200;

int64_t NowMs()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

class RedisSweepTimer : public ::OI::Timer::BaseTimer
{
public:
	bool OnTimer()
	{
		::OI::Net::Poll::GetInstance().CallOnLoop(RedisClient::SweepTimeout, NULL);
		return true;
	}
};
}

struct RedisCall::State
{
	std::vector<std::string> vArguments;
	std::shared_ptr<redisReply> pReply;
	RedisStatus iStatus;
	bool bReady;
	bool bSynchronous;
	RedisResult* pOutput;
	std::coroutine_handle<> stHandle;
	int64_t iDeadlineMilliseconds;
	std::atomic<int32_t> iGate;
	std::atomic<int32_t> iWon;
	std::atomic<int32_t> iReferences;
	sid_t iSessionId;
	::OI::Thread::Mutex stMutex;
	::OI::Thread::MutexCond stConditionVariable;
	State() : iStatus(RedisStatus::Disconnected), bReady(false), bSynchronous(false), pOutput(NULL), iDeadlineMilliseconds(0), iGate(0), iWon(0), iReferences(1), iSessionId(INVALID_SID) {}
};

RedisClient::RedisClient() : m_pReader(NULL), m_iTimeoutMilliseconds(25000), m_iMaxPending(65536), m_iSwept(0)
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

bool RedisClient::Init()
{
	TryGetConfig("RedisClient.timeout_ms", m_iTimeoutMilliseconds);
	TryGetConfig("RedisClient.max_pending", m_iMaxPending);
	if (m_iMaxPending < 1) return false;
	if (m_iTimeoutMilliseconds <= 0)
	{
		m_iTimeoutMilliseconds = 25000;
	}
	if (!::OI::Rpc::ProtoClientManager::Init()) return false;
	return ::OI::Thread::ThreadPool::GetInstance().AddStopHandler([]() {
		auto& stClient = GetInstance();
		stClient.FailAll();
		stClient.m_iReaderSessionId = INVALID_SID;
		stClient.CloseAllSessions();
	});
}

::OI::Net::Session* RedisClient::CloneSession()
{
	return new RedisClientSession(this);
}

void RedisClient::OnClientConnect(::OI::Net::Session* pSessionParam)
{
	// 新连接发布前先结束旧等待；旧连接的后续回包由 Feed 的 sid 检查丢弃。
	FailAll();
	m_iReaderSessionId = pSessionParam->GetSessionID();
	if (m_pReader)
	{
		redisReaderFree(m_pReader);
	}
	m_pReader = redisReaderCreate();
	LOG_TRACE("RedisClient::OnClientConnect sid=%d", m_iReaderSessionId);
}

void RedisClient::OnClientDisconnect(const ::OI::Net::Session* pSessionParam)
{
	sid_t iSessionIdLocal = pSessionParam->GetSessionID();
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

void RedisClient::Release(RedisCall::State* pStParam)
{
	if (!pStParam)
	{
		return;
	}
	if (pStParam->iReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete pStParam;
	}
}

RedisCall::RedisCall(std::vector<std::string> vArgumentsParam, RedisResult* pOutputParam) : m_pState(new State())
{
	m_pState->vArguments = std::move(vArgumentsParam);
	m_pState->pOutput = pOutputParam;
}

RedisCall::RedisCall(RedisCall&& stOtherParam) noexcept : m_pState(stOtherParam.m_pState)
{
	stOtherParam.m_pState = NULL;
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
	if (m_pState->pOutput) *m_pState->pOutput = stResultLocal;
	return stResultLocal;
}

bool RedisCall::Suspend(std::coroutine_handle<> stHandleParam)
{
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested()) return false;
	m_pState->stHandle = stHandleParam;
	::OI::Net::Poll::GetInstance().CallOnLoop(RedisClient::Submit, m_pState);
	int iPrevious = m_pState->iGate.exchange(2, std::memory_order_acq_rel);
	return iPrevious != 1;
}

RedisCall RedisClient::Call(std::vector<std::string> vArgumentsParam, RedisResult* pOutputParam)
{
	return RedisCall(std::move(vArgumentsParam), pOutputParam);
}

RedisResult RedisClient::CallSync(std::vector<std::string> vArgumentsParam)
{
	// 两种模式都禁止在 libuv 回调中同步等待，不能重入事件循环。
	if (::OI::Net::Poll::GetInstance().InPollCallback())
		throw std::logic_error("CallRedisSync cannot run in a poll callback; use co_await CallRedis");
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested()) return {};
	RedisCall::State* pSt = new RedisCall::State();
	pSt->vArguments = std::move(vArgumentsParam);
	pSt->bSynchronous = true;
	::OI::Net::Poll::GetInstance().CallOnLoop(Submit, pSt);
#ifdef _SINGLE_THREAD_
	while (!pSt->bReady && !::OI::Thread::ThreadPool::GetInstance().Stopping())
	{
		// 同步调用也可能来自定时回调；不能依赖该回调返回后才能继续推进的时间轮。
		SweepTimeout(nullptr);
		if (!pSt->bReady) ::OI::Thread::ThreadPool::GetInstance().PumpOnce();
	}
	if (!pSt->bReady) Settle(pSt, {}, RedisStatus::Disconnected);
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

void RedisClient::Submit(void* pStateParam)
{
	GetInstance().Enqueue((RedisCall::State*)pStateParam);
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

void RedisClient::Settle(RedisCall::State* pStParam, std::shared_ptr<redisReply> pReplyParam, RedisStatus iStatusParam)
{
	int iExpect = 0;
	if (!pStParam->iWon.compare_exchange_strong(iExpect, 1, std::memory_order_acq_rel))
	{
		return;
	}
	const bool bSync = pStParam->bSynchronous;
	{
		::OI::Thread::MutexGuard stLock(pStParam->stMutex);
		pStParam->pReply = std::move(pReplyParam);
		pStParam->iStatus = iStatusParam;
		pStParam->bReady = true;
		if (bSync)
		{
			pStParam->stConditionVariable.Signal();
		}
	}
	// 同步调用可能在通知后立即释放状态，出锁后不再解引用它。
	if (bSync)
	{
		return;
	}
	int iPrevious = pStParam->iGate.exchange(1, std::memory_order_acq_rel);
	if (iPrevious == 2 && pStParam->stHandle)
	{
		coro::Schedule(pStParam->stHandle);
	}
}

void RedisClient::Enqueue(RedisCall::State* pStParam)
{
	if (pStParam->vArguments.empty())
	{
		Settle(pStParam, {}, RedisStatus::InvalidArgument);
		return;
	}
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested()
		|| m_iReaderSessionId == INVALID_SID || ConnectedSid() != m_iReaderSessionId)
	{
		Settle(pStParam, {}, RedisStatus::Disconnected);
		return;
	}
	std::vector<const char*> vArgumentsLocal(pStParam->vArguments.size());
	std::vector<size_t> vArglen(pStParam->vArguments.size());
	for (size_t iIndexLocal = 0; iIndexLocal < pStParam->vArguments.size(); ++iIndexLocal)
	{
		vArgumentsLocal[iIndexLocal] = pStParam->vArguments[iIndexLocal].c_str();
		vArglen[iIndexLocal] = pStParam->vArguments[iIndexLocal].size();
	}
	char* pCmd = NULL;
	long long iNumber = redisFormatCommandArgv(&pCmd, (int)vArgumentsLocal.size(), vArgumentsLocal.data(), vArglen.data());
	if (iNumber < 0 || !pCmd)
	{
		Settle(pStParam, {}, RedisStatus::InvalidArgument);
		return;
	}
	::OI::Base::Bytes stFrame;
	stFrame.Append(pCmd, (size_t)iNumber);
	redisFreeCommand(pCmd);
	std::call_once(g_sweep_once, []() {
		::OI::Timer::BaseTimerManager::GetInstance().AddTimer(new RedisSweepTimer(), kSweepMs);
	});
	pStParam->iDeadlineMilliseconds = NowMs() + m_iTimeoutMilliseconds;
	bool bQueued = false;
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		// 超时的也占着位置。满了就不再排。
		if (m_vWait.size() < static_cast<size_t>(m_iMaxPending))
		{
			pStParam->iSessionId = m_iReaderSessionId;
			pStParam->iReferences.fetch_add(1, std::memory_order_relaxed);
			m_vWait.push_back(pStParam);
			bQueued = true;
		}
	}
	if (!bQueued)
	{
		Settle(pStParam, {}, RedisStatus::Busy);
		return;
	}
	if (!SendBytes(pStParam->iSessionId, stFrame))
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		for (size_t iIndexLocal = 0; iIndexLocal < m_vWait.size(); ++iIndexLocal)
		{
			if (m_vWait[iIndexLocal] == pStParam)
			{
				m_vWait.erase(m_vWait.begin() + (std::ptrdiff_t)iIndexLocal);
				if (iIndexLocal < m_iSwept)
				{
					--m_iSwept;
				}
				break;
			}
		}
		Settle(pStParam, {}, RedisStatus::SendFailed);
		Release(pStParam);
		return;
	}
}

void RedisClient::FailSid(sid_t iSessionIdParam)
{
	std::deque<RedisCall::State*> vLeft;
	{
		::OI::Thread::MutexGuard stLock(m_stWaitMutex);
		std::deque<RedisCall::State*> vKeep;
		size_t iSwept = 0;
		for (size_t iIndexLocal = 0; iIndexLocal < m_vWait.size(); ++iIndexLocal)
		{
			if (m_vWait[iIndexLocal]->iSessionId == iSessionIdParam)
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

void RedisClient::FailAll(RedisStatus iStatusParam)
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
			Settle(vLeft[iIndexLocal], {}, iStatusParam);
		}
		Release(vLeft[iIndexLocal]);
	}
}

void RedisClient::Feed(sid_t iSessionIdParam, const char* pDataParam, size_t iSizeParam)
{
	if (iSessionIdParam != m_iReaderSessionId || iSessionIdParam == INVALID_SID) return;
	if (!m_pReader)
	{
		m_pReader = redisReaderCreate();
	}
	if (redisReaderFeed(m_pReader, pDataParam, iSizeParam) != REDIS_OK)
	{
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
		if (pSt->iWon.load(std::memory_order_acquire) == 0)
		{
			const auto iStatusLocal = pHeld->type == REDIS_REPLY_ERROR ? RedisStatus::ServerError : RedisStatus::Ok;
			Settle(pSt, std::move(pHeld), iStatusLocal);
		}
		Release(pSt);
	}
}
}}
