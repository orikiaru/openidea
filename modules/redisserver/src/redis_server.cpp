#include "redis_server.h"
#include "redis_server_session.h"
#include "hiredis.h"
#include "async.h"
#include "oi_channel.h"
#include "oi_poll.h"
#include "oi_socket.h"
#include "oi_timer_manager.h"
#include "oi_config.h"
#include "oi_thread.h"
#include "oi_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <algorithm>

namespace
{
class RedisEv : public ::OI::Net::Channel
{
	redisAsyncContext* m_pAsyncContext;
	int32_t m_iMask;
	bool m_bInIo;
	bool m_bDead;

  public:
	RedisEv(::OI::Net::NetSocket stSockParam, redisAsyncContext* pAsyncContextParam)
	    : ::OI::Net::Channel(stSockParam, NULL), m_pAsyncContext(pAsyncContextParam), m_iMask(0), m_bInIo(false), m_bDead(false)
	{
	}
	void DataIn()
	{
		m_bInIo = true;
		if (m_pAsyncContext)
		{
			redisAsyncHandleRead(m_pAsyncContext);
		}
	}
	void DataOut()
	{
		m_bInIo = true;
		if (m_pAsyncContext)
		{
			redisAsyncHandleWrite(m_pAsyncContext);
		}
	}
	bool TryClose()
	{
		if (!m_bDead)
		{
			m_bInIo = false;
			return false;
		}
		::OI::Net::Poll::GetInstance().IOEventUnRegister(this, false);
		delete this;
		return true;
	}
	void Add(int32_t iBitParam)
	{
		m_iMask |= iBitParam;
		Touch();
	}
	void Del(int32_t iBitParam)
	{
		m_iMask &= ~iBitParam;
		Touch();
	}
	void Touch()
	{
		if (m_bDead)
		{
			return;
		}
		if (pPollToken == NULL)
		{
			if (m_iMask == 0)
			{
				return;
			}
			::OI::Net::Poll::GetInstance().IOEventRegister(this,
			    (m_iMask & ::OI::Net::IO_EV_READ) != 0, (m_iMask & ::OI::Net::IO_EV_WRITE) != 0);
			return;
		}
		::OI::Net::Poll::GetInstance().IOEventMod(this, m_iMask);
	}
	void Cleanup()
	{
		m_pAsyncContext = NULL;
		m_bDead = true;
		if (m_bInIo)
		{
			return;
		}
		::OI::Net::Poll::GetInstance().IOEventUnRegister(this, false);
		delete this;
	}
};

void EvAddRead(void* pPointerParam)
{
	((RedisEv*)pPointerParam)->Add(::OI::Net::IO_EV_READ);
}
void EvDelRead(void* pPointerParam)
{
	((RedisEv*)pPointerParam)->Del(::OI::Net::IO_EV_READ);
}
void EvAddWrite(void* pPointerParam)
{
	((RedisEv*)pPointerParam)->Add(::OI::Net::IO_EV_WRITE);
}
void EvDelWrite(void* pPointerParam)
{
	((RedisEv*)pPointerParam)->Del(::OI::Net::IO_EV_WRITE);
}
void EvCleanup(void* pPointerParam)
{
	((RedisEv*)pPointerParam)->Cleanup();
}

int AttachRedis(redisAsyncContext* pAsyncContextParam)
{
	if (!pAsyncContextParam || pAsyncContextParam->ev.data != NULL)
	{
		return REDIS_ERR;
	}
	::OI::Net::NetSocket stSock;
	stSock.Attach(pAsyncContextParam->c.fd);
	RedisEv* pEv = new RedisEv(stSock, pAsyncContextParam);
	pAsyncContextParam->ev.addRead = EvAddRead;
	pAsyncContextParam->ev.delRead = EvDelRead;
	pAsyncContextParam->ev.addWrite = EvAddWrite;
	pAsyncContextParam->ev.delWrite = EvDelWrite;
	pAsyncContextParam->ev.cleanup = EvCleanup;
	pAsyncContextParam->ev.data = pEv;
	return REDIS_OK;
}

void AppendBulk(::OI::Base::Bytes& stOutputParam, const char* pTextParam, size_t iNumberParam)
{
	char vHdr[64];
	int iHn = snprintf(vHdr, sizeof(vHdr), "$%zu\r\n", iNumberParam);
	stOutputParam.Append(vHdr, (size_t)iHn);
	if (iNumberParam && pTextParam)
	{
		stOutputParam.Append(pTextParam, iNumberParam);
	}
	stOutputParam.Append("\r\n", 2);
}

void AppendReply(::OI::Base::Bytes& stOutputParam, const redisReply* pReplyParam)
{
	if (!pReplyParam)
	{
		stOutputParam.Append("$-1\r\n", 5);
		return;
	}
	char vHdr[64];
	int iHn = 0;
	switch (pReplyParam->type)
	{
	case REDIS_REPLY_STATUS:
		stOutputParam.Append("+", 1);
		if (pReplyParam->str)
		{
			stOutputParam.Append(pReplyParam->str, pReplyParam->len);
		}
		stOutputParam.Append("\r\n", 2);
		break;
	case REDIS_REPLY_ERROR:
		stOutputParam.Append("-", 1);
		if (pReplyParam->str)
		{
			stOutputParam.Append(pReplyParam->str, pReplyParam->len);
		}
		stOutputParam.Append("\r\n", 2);
		break;
	case REDIS_REPLY_INTEGER:
	case REDIS_REPLY_BOOL:
		iHn = snprintf(vHdr, sizeof(vHdr), ":%lld\r\n", pReplyParam->integer);
		stOutputParam.Append(vHdr, (size_t)iHn);
		break;
	case REDIS_REPLY_NIL:
		stOutputParam.Append("$-1\r\n", 5);
		break;
	case REDIS_REPLY_STRING:
		AppendBulk(stOutputParam, pReplyParam->str, pReplyParam->len);
		break;
	case REDIS_REPLY_ARRAY:
		iHn = snprintf(vHdr, sizeof(vHdr), "*%zu\r\n", pReplyParam->elements);
		stOutputParam.Append(vHdr, (size_t)iHn);
		for (size_t iIndexLocal = 0; iIndexLocal < pReplyParam->elements; ++iIndexLocal)
		{
			AppendReply(stOutputParam, pReplyParam->element[iIndexLocal]);
		}
		break;
	default:
		stOutputParam.Append("-ERR reply\r\n", 12);
		break;
	}
}

void AppendErr(::OI::Base::Bytes& stOutputParam, const char* pTextParam)
{
	stOutputParam.Append("-", 1);
	stOutputParam.Append(pTextParam, strlen(pTextParam));
	stOutputParam.Append("\r\n", 2);
}

void OnReply(redisAsyncContext*, void* pReplyParam, void* pPrivParam)
{
	auto* pLink = static_cast<RedisLink*>(pPrivParam);
	if (pLink->vPending.empty())
	{
		if (pReplyParam)
		{
			freeReplyObject(pReplyParam);
		}
		return;
	}
	const sid_t iSessionIdLocal = pLink->vPending.front().iSessionId;
	// 先归还额度，恢复读取延后到下一轮，避免在 hiredis 回调里重入解析。
	RedisServer::GetInstance().ReleaseCredit(pLink);
	// 单线程进程，回包缓冲复用一块，不必每条回执分配。
	static ::OI::Base::Bytes stOutput;
	stOutput.Clear();
	if (!pReplyParam)
	{
		AppendErr(stOutput, "ERR redis down");
	}
	else
	{
		AppendReply(stOutput, (redisReply*)pReplyParam);
		freeReplyObject(pReplyParam);
	}
	if (!RedisServer::GetInstance().SendBytes(iSessionIdLocal, stOutput))
	{
		RedisServer::GetInstance().CloseSession(iSessionIdLocal);
	}
}

void OnConnect(redisAsyncContext* pCharacterParam, int iStatusParam)
{
	RedisLink* pLink = (RedisLink*)pCharacterParam->data;
	if (iStatusParam != REDIS_OK)
	{
		LOG_ERROR("RedisServer hiredis connect %s", pCharacterParam->c.errstr);
		pLink->pAsyncContext = NULL;
		RedisServer::GetInstance().ArmReconnect(pLink);
		return;
	}
	LOG_TRACE("RedisServer hiredis up index=%d", pLink->iIndex);
}

void OnDisconnect(const redisAsyncContext* pCharacterParam, int iStatusParam)
{
	(void)iStatusParam;
	RedisLink* pLink = (RedisLink*)pCharacterParam->data;
	LOG_TRACE("RedisServer hiredis down index=%d", pLink->iIndex);
	pLink->pAsyncContext = NULL;
	RedisServer::GetInstance().ArmReconnect(pLink);
}
}

RedisServer::RedisServer()
    : m_iConnections(1), m_iNext(0)
{
}

RedisServer::~RedisServer()
{
	for (size_t iIndexLocal = 0; iIndexLocal < m_vLinks.size(); ++iIndexLocal)
	{
		if (m_vLinks[iIndexLocal].pAsyncContext)
		{
			redisAsyncFree(m_vLinks[iIndexLocal].pAsyncContext);
			m_vLinks[iIndexLocal].pAsyncContext = NULL;
		}
	}
}

bool RedisServer::Init()
{
	if (!::OI::Net::SessionManager::Init())
	{
		return false;
	}
	if (!TryGetConfig("Redis.ip", m_sRedisIp) || !TryGetConfig("Redis.port", m_sRedisPort))
	{
		LOG_ERROR("RedisServer missing Redis.ip or Redis.port");
		return false;
	}
	int64_t iMaxBytes = static_cast<int64_t>(m_iMaxPendingBytes);
	int64_t iMaxCommands = static_cast<int64_t>(m_iMaxPendingCommands);
	TryGetConfig("Redis.max_pending_bytes", iMaxBytes);
	TryGetConfig("Redis.max_pending_commands", iMaxCommands);
	if (iMaxBytes < 64 || static_cast<uint64_t>(iMaxBytes) > (std::numeric_limits<size_t>::max)() || iMaxCommands < 1 || static_cast<uint64_t>(iMaxCommands) > (std::numeric_limits<size_t>::max)())
	{
		return false;
	}
	m_iMaxPendingBytes = static_cast<size_t>(iMaxBytes);
	m_iMaxPendingCommands = static_cast<size_t>(iMaxCommands);
	m_iConnections = 1;
	TryGetConfig("Redis.connections", m_iConnections);
	if (m_iConnections < 1)
	{
		m_iConnections = 1;
	}
	m_vLinks.resize((size_t)m_iConnections);
	for (int iIndexLocal = 0; iIndexLocal < m_iConnections; ++iIndexLocal)
	{
		m_vLinks[(size_t)iIndexLocal].iIndex = iIndexLocal;
		Open(&m_vLinks[(size_t)iIndexLocal]);
	}
	LOG_TRACE("RedisServer connections=%d", m_iConnections);
	return true;
}

::OI::Net::Session* RedisServer::CloneSession()
{
	return new RedisServerSession(this);
}

void RedisServer::OnAddSession(::OI::Net::Session* pSessionParam)
{
	int iNumber = 0;
	if (!m_vLinks.empty())
	{
		iNumber = m_iNext % (int)m_vLinks.size();
		++m_iNext;
	}
	((RedisServerSession*)pSessionParam)->Bind(iNumber);
	LOG_TRACE("RedisServer::OnAddSession sid=%d link=%d", pSessionParam->GetSessionID(), iNumber);
}

void RedisServer::OnDelSession(const ::OI::Net::Session* pSessionParam)
{
	for (auto& stLink : m_vLinks)
	{
		stLink.vPaused.erase(pSessionParam->GetSessionID());
		std::erase(stLink.vReady, pSessionParam->GetSessionID());
	}
	LOG_TRACE("RedisServer::OnDelSession sid=%d", pSessionParam->GetSessionID());
}

void RedisServer::Open(RedisLink* pLinkParam)
{
	if (pLinkParam->pAsyncContext)
	{
		return;
	}
	redisAsyncContext* pAsyncContextLocal = redisAsyncConnect(m_sRedisIp.c_str(), std::atoi(m_sRedisPort.c_str()));
	if (!pAsyncContextLocal)
	{
		ArmReconnect(pLinkParam);
		return;
	}
	pAsyncContextLocal->data = pLinkParam;
	pAsyncContextLocal->c.flags |= REDIS_NO_AUTO_FREE_REPLIES;
	if (pAsyncContextLocal->err)
	{
		LOG_ERROR("RedisServer hiredis %s", pAsyncContextLocal->c.errstr);
		redisAsyncFree(pAsyncContextLocal);
		ArmReconnect(pLinkParam);
		return;
	}
	if (AttachRedis(pAsyncContextLocal) != REDIS_OK)
	{
		LOG_ERROR("RedisServer hiredis attach");
		redisAsyncFree(pAsyncContextLocal);
		ArmReconnect(pLinkParam);
		return;
	}
	pLinkParam->pAsyncContext = pAsyncContextLocal;
	redisAsyncSetConnectCallbackNC(pAsyncContextLocal, OnConnect);
	redisAsyncSetDisconnectCallback(pAsyncContextLocal, OnDisconnect);
}

void RedisServer::ArmReconnect(RedisLink* pLinkParam)
{
	if (pLinkParam->bDialing)
	{
		return;
	}
	pLinkParam->bDialing = true;
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([pLinkParam]()
	{
		pLinkParam->bDialing = false;
		RedisServer::GetInstance().Open(pLinkParam);
	}, 2000);
}

void RedisServer::Pause(int32_t iLinkParam, sid_t iSessionIdParam)
{
	auto& stLink = m_vLinks.at(static_cast<size_t>(iLinkParam));
	if (stLink.vPaused.insert(iSessionIdParam).second)
	{
		stLink.vReady.push_back(iSessionIdParam);
		LOG_TRACE("redis pressure pause link=%d pending=%zu bytes=%zu", iLinkParam, stLink.vPending.size(), stLink.iPendingBytes);
		fflush(stdout);
	}
	if (!stLink.bResuming)
	{
		ArmResume(&stLink);
	}
}

void RedisServer::ReleaseCredit(RedisLink* pLinkParam)
{
	pLinkParam->iPendingBytes -= pLinkParam->vPending.front().iBytes;
	pLinkParam->vPending.pop_front();
	ArmResume(pLinkParam);
}

void RedisServer::ArmResume(RedisLink* pLinkParam)
{
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested() || pLinkParam->bResumeArmed || pLinkParam->vPaused.empty() || pLinkParam->iPendingBytes > m_iMaxPendingBytes / 2 || pLinkParam->vPending.size() > m_iMaxPendingCommands / 2)
	{
		return;
	}
	pLinkParam->bResumeArmed = true;
	OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgumentParam)
	{
		RedisServer::GetInstance().Resume(static_cast<RedisLink*>(pArgumentParam));
	}, pLinkParam);
}

void RedisServer::Resume(RedisLink* pLinkParam)
{
	pLinkParam->bResumeArmed = false;
	pLinkParam->bResuming = true;
	bool bProgress = false;
	size_t iRemaining = pLinkParam->vReady.size();
	while (iRemaining-- > 0 && !pLinkParam->vReady.empty())
	{
		// 额度满时保留队首；恢复者每次只发一条，然后排到队尾。
		if (pLinkParam->vPending.size() >= m_iMaxPendingCommands || pLinkParam->iPendingBytes >= m_iMaxPendingBytes)
		{
			break;
		}
		const sid_t iSessionId = pLinkParam->vReady.front();
		pLinkParam->vReady.pop_front();
		pLinkParam->vPaused.erase(iSessionId);
		RedisServerSession* pSession = nullptr;
		{
			::OI::Thread::MutexGuard stLock(stLocker);
			auto it = mSessionMap.find(iSessionId);
			if (it != mSessionMap.end())
			{
				pSession = static_cast<RedisServerSession*>(it->second);
				pSession->AddRef();
			}
		}
		if (pSession)
		{
			const bool bConsumed = pSession->ResumeRead();
			pSession->Release();
			bProgress = bProgress || bConsumed;
			if (!bConsumed && pLinkParam->vPaused.contains(iSessionId))
			{
				// 大请求额度尚不足时保留优先权，不能被后续小请求永久挤占。
				std::erase(pLinkParam->vReady, iSessionId);
				pLinkParam->vReady.push_front(iSessionId);
				break;
			}
		}
	}
	pLinkParam->bResuming = false;
	if (bProgress)
	{
		ArmResume(pLinkParam);
	}
}

bool RedisServer::Issue(int32_t iLinkParam, sid_t iSessionIdParam, const char* pCmdParam, size_t iLengthParam)
{
	if (iLinkParam < 0 || iLinkParam >= static_cast<int>(m_vLinks.size()) || iLengthParam == 0 || iLengthParam > m_iMaxPendingBytes)
	{
		CloseSession(iSessionIdParam);
		return true;
	}
	auto& stLink = m_vLinks[static_cast<size_t>(iLinkParam)];
	if (!stLink.pAsyncContext)
	{
		::OI::Base::Bytes stError;
		AppendErr(stError, "ERR redis down");
		if (!SendBytes(iSessionIdParam, stError))
		{
			CloseSession(iSessionIdParam);
		}
		return true;
	}
	if (!stLink.bResuming && !stLink.vPaused.empty())
	{
		return false;
	}
	if (stLink.vPending.size() >= m_iMaxPendingCommands || iLengthParam > m_iMaxPendingBytes - stLink.iPendingBytes)
	{
		return false;
	}
	stLink.vPending.push_back(RedisPending{iSessionIdParam, iLengthParam});
	stLink.iPendingBytes += iLengthParam;
	if (redisAsyncFormattedCommand(stLink.pAsyncContext, OnReply, &stLink, pCmdParam, iLengthParam) != REDIS_OK)
	{
		stLink.iPendingBytes -= iLengthParam;
		stLink.vPending.pop_back();
		// 本地失败不能插队回包；关闭下游，已发出的旧命令仍按 FIFO 结算额度。
		CloseSession(iSessionIdParam);
	}
	return true;
}
