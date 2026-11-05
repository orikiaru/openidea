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
	RedisEv(::OI::Net::NetSocket stSock, redisAsyncContext* pAsyncContext)
	    : ::OI::Net::Channel(stSock, NULL), m_pAsyncContext(pAsyncContext), m_iMask(0), m_bInIo(false), m_bDead(false)
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
	void Add(int32_t iBit)
	{
		m_iMask |= iBit;
		Touch();
	}
	void Del(int32_t iBit)
	{
		m_iMask &= ~iBit;
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

void EvAddRead(void* pPointer)
{
	((RedisEv*)pPointer)->Add(::OI::Net::IO_EV_READ);
}
void EvDelRead(void* pPointer)
{
	((RedisEv*)pPointer)->Del(::OI::Net::IO_EV_READ);
}
void EvAddWrite(void* pPointer)
{
	((RedisEv*)pPointer)->Add(::OI::Net::IO_EV_WRITE);
}
void EvDelWrite(void* pPointer)
{
	((RedisEv*)pPointer)->Del(::OI::Net::IO_EV_WRITE);
}
void EvCleanup(void* pPointer)
{
	((RedisEv*)pPointer)->Cleanup();
}

int AttachRedis(redisAsyncContext* pAsyncContext)
{
	if (!pAsyncContext || pAsyncContext->ev.data != NULL)
	{
		return REDIS_ERR;
	}
	::OI::Net::NetSocket stSock;
	stSock.Attach(pAsyncContext->c.fd);
	RedisEv* pEv = new RedisEv(stSock, pAsyncContext);
	pAsyncContext->ev.addRead = EvAddRead;
	pAsyncContext->ev.delRead = EvDelRead;
	pAsyncContext->ev.addWrite = EvAddWrite;
	pAsyncContext->ev.delWrite = EvDelWrite;
	pAsyncContext->ev.cleanup = EvCleanup;
	pAsyncContext->ev.data = pEv;
	return REDIS_OK;
}

void AppendBulk(::OI::Base::Bytes& stOutput, const char* pText, size_t iNumber)
{
	char vHdr[64];
	int iHn = snprintf(vHdr, sizeof(vHdr), "$%zu\r\n", iNumber);
	stOutput.Append(vHdr, (size_t)iHn);
	if (iNumber && pText)
	{
		stOutput.Append(pText, iNumber);
	}
	stOutput.Append("\r\n", 2);
}

void AppendReply(::OI::Base::Bytes& stOutput, const redisReply* pReply)
{
	if (!pReply)
	{
		stOutput.Append("$-1\r\n", 5);
		return;
	}
	char vHdr[64];
	int iHn = 0;
	switch (pReply->type)
	{
	case REDIS_REPLY_STATUS:
		stOutput.Append("+", 1);
		if (pReply->str)
		{
			stOutput.Append(pReply->str, pReply->len);
		}
		stOutput.Append("\r\n", 2);
		break;
	case REDIS_REPLY_ERROR:
		stOutput.Append("-", 1);
		if (pReply->str)
		{
			stOutput.Append(pReply->str, pReply->len);
		}
		stOutput.Append("\r\n", 2);
		break;
	case REDIS_REPLY_INTEGER:
	case REDIS_REPLY_BOOL:
		iHn = snprintf(vHdr, sizeof(vHdr), ":%lld\r\n", pReply->integer);
		stOutput.Append(vHdr, (size_t)iHn);
		break;
	case REDIS_REPLY_NIL:
		stOutput.Append("$-1\r\n", 5);
		break;
	case REDIS_REPLY_STRING:
		AppendBulk(stOutput, pReply->str, pReply->len);
		break;
	case REDIS_REPLY_ARRAY:
		iHn = snprintf(vHdr, sizeof(vHdr), "*%zu\r\n", pReply->elements);
		stOutput.Append(vHdr, (size_t)iHn);
		for (size_t iIndexLocal = 0; iIndexLocal < pReply->elements; ++iIndexLocal)
		{
			AppendReply(stOutput, pReply->element[iIndexLocal]);
		}
		break;
	default:
		stOutput.Append("-ERR reply\r\n", 12);
		break;
	}
}

void AppendErr(::OI::Base::Bytes& stOutput, const char* pText)
{
	stOutput.Append("-", 1);
	stOutput.Append(pText, strlen(pText));
	stOutput.Append("\r\n", 2);
}

void OnReply(redisAsyncContext*, void* pReply, void* pPriv)
{
	auto* pLink = static_cast<RedisLink*>(pPriv);
	if (pLink->vPending.empty())
	{
		if (pReply)
		{
			freeReplyObject(pReply);
		}
		return;
	}
	const sid_t iSessionIdLocal = pLink->vPending.front().iSessionId;
	// 先归还额度，恢复读取延后到下一轮，避免在 hiredis 回调里重入解析。
	RedisServer::GetInstance().ReleaseCredit(pLink);
	// 单线程进程，回包缓冲复用一块，不必每条回执分配。
	static ::OI::Base::Bytes stOutput;
	stOutput.Clear();
	if (!pReply)
	{
		AppendErr(stOutput, "ERR redis down");
	}
	else
	{
		AppendReply(stOutput, (redisReply*)pReply);
		freeReplyObject(pReply);
	}
	if (!RedisServer::GetInstance().SendBytes(iSessionIdLocal, stOutput))
	{
		RedisServer::GetInstance().CloseSession(iSessionIdLocal);
	}
}

void OnConnect(redisAsyncContext* pCharacter, int iStatus)
{
	RedisLink* pLink = (RedisLink*)pCharacter->data;
	if (iStatus != REDIS_OK)
	{
		LOG_ERROR("RedisServer hiredis connect %s", pCharacter->c.errstr);
		pLink->pAsyncContext = NULL;
		RedisServer::GetInstance().ArmReconnect(pLink);
		return;
	}
	LOG_TRACE("RedisServer hiredis up index=%d", pLink->iIndex);
}

void OnDisconnect(const redisAsyncContext* pCharacter, int iStatus)
{
	RedisLink* pLink = (RedisLink*)pCharacter->data;
	// 在 hiredis 释放 context 前保留原始原因；日志失败不能阻止重连。
	const OI::Log::DiagnosticText stReason(pCharacter->c.errstr);
	const OI::Log::DiagnosticText stHost(pCharacter->c.tcp.host ? pCharacter->c.tcp.host : "unknown");
	try
	{
		LOG_ERROR("redis upstream disconnect index=%d host=%s port=%d status=%d error=%d reason=%s truncated=%d",
		    pLink->iIndex, stHost.GetText(), pCharacter->c.tcp.port, iStatus, pCharacter->c.err,
		    stReason.GetText(), stReason.IsTruncated() ? 1 : 0);
		fflush(stdout);
	}
	catch (...)
	{
		fprintf(stderr, "redis upstream disconnect index=%d host=%s port=%d status=%d error=%d reason=%s truncated=%d logger_failed=1\n",
		    pLink->iIndex, stHost.GetText(), pCharacter->c.tcp.port, iStatus, pCharacter->c.err,
		    stReason.GetText(), stReason.IsTruncated() ? 1 : 0);
	}
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

void RedisServer::OnAddSession(::OI::Net::Session* pSession)
{
	int iNumber = 0;
	if (!m_vLinks.empty())
	{
		iNumber = m_iNext % (int)m_vLinks.size();
		++m_iNext;
	}
	((RedisServerSession*)pSession)->Bind(iNumber);
	LOG_TRACE("RedisServer::OnAddSession sid=%d link=%d", pSession->GetSessionID(), iNumber);
}

void RedisServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	for (auto& stLink : m_vLinks)
	{
		stLink.vPaused.erase(pSession->GetSessionID());
		std::erase(stLink.vReady, pSession->GetSessionID());
	}
	LOG_TRACE("RedisServer::OnDelSession sid=%d", pSession->GetSessionID());
}

void RedisServer::Open(RedisLink* pLink)
{
	if (pLink->pAsyncContext)
	{
		return;
	}
	redisAsyncContext* pAsyncContextLocal = redisAsyncConnect(m_sRedisIp.c_str(), std::atoi(m_sRedisPort.c_str()));
	if (!pAsyncContextLocal)
	{
		ArmReconnect(pLink);
		return;
	}
	pAsyncContextLocal->data = pLink;
	pAsyncContextLocal->c.flags |= REDIS_NO_AUTO_FREE_REPLIES;
	if (pAsyncContextLocal->err)
	{
		LOG_ERROR("RedisServer hiredis %s", pAsyncContextLocal->c.errstr);
		redisAsyncFree(pAsyncContextLocal);
		ArmReconnect(pLink);
		return;
	}
	if (AttachRedis(pAsyncContextLocal) != REDIS_OK)
	{
		LOG_ERROR("RedisServer hiredis attach");
		redisAsyncFree(pAsyncContextLocal);
		ArmReconnect(pLink);
		return;
	}
	pLink->pAsyncContext = pAsyncContextLocal;
	redisAsyncSetConnectCallbackNC(pAsyncContextLocal, OnConnect);
	redisAsyncSetDisconnectCallback(pAsyncContextLocal, OnDisconnect);
}

void RedisServer::ArmReconnect(RedisLink* pLink)
{
	if (pLink->bDialing)
	{
		return;
	}
	pLink->bDialing = true;
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([pLink]()
	{
		pLink->bDialing = false;
		RedisServer::GetInstance().Open(pLink);
	}, 2000);
}

void RedisServer::Pause(int32_t iLink, sid_t iSessionId)
{
	auto& stLink = m_vLinks.at(static_cast<size_t>(iLink));
	if (stLink.vPaused.insert(iSessionId).second)
	{
		stLink.vReady.push_back(iSessionId);
		LOG_TRACE("redis pressure pause link=%d pending=%zu bytes=%zu", iLink, stLink.vPending.size(), stLink.iPendingBytes);
		fflush(stdout);
	}
	if (!stLink.bResuming)
	{
		ArmResume(&stLink);
	}
}

void RedisServer::ReleaseCredit(RedisLink* pLink)
{
	pLink->iPendingBytes -= pLink->vPending.front().iBytes;
	pLink->vPending.pop_front();
	ArmResume(pLink);
}

void RedisServer::ArmResume(RedisLink* pLink)
{
	if (::OI::Thread::ThreadPool::GetInstance().StopRequested() || pLink->bResumeArmed || pLink->vPaused.empty() || pLink->iPendingBytes > m_iMaxPendingBytes / 2 || pLink->vPending.size() > m_iMaxPendingCommands / 2)
	{
		return;
	}
	pLink->bResumeArmed = true;
	OI::Net::Poll::GetInstance().PostOnLoop([](void* pArgument)
	{
		RedisServer::GetInstance().Resume(static_cast<RedisLink*>(pArgument));
	}, pLink);
}

void RedisServer::Resume(RedisLink* pLink)
{
	pLink->bResumeArmed = false;
	pLink->bResuming = true;
	bool bProgress = false;
	size_t iRemaining = pLink->vReady.size();
	while (iRemaining-- > 0 && !pLink->vReady.empty())
	{
		// 额度满时保留队首；恢复者每次只发一条，然后排到队尾。
		if (pLink->vPending.size() >= m_iMaxPendingCommands || pLink->iPendingBytes >= m_iMaxPendingBytes)
		{
			break;
		}
		const sid_t iSessionId = pLink->vReady.front();
		pLink->vReady.pop_front();
		pLink->vPaused.erase(iSessionId);
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
			if (!bConsumed && pLink->vPaused.contains(iSessionId))
			{
				// 大请求额度尚不足时保留优先权，不能被后续小请求永久挤占。
				std::erase(pLink->vReady, iSessionId);
				pLink->vReady.push_front(iSessionId);
				break;
			}
		}
	}
	pLink->bResuming = false;
	if (bProgress)
	{
		ArmResume(pLink);
	}
}

bool RedisServer::Issue(int32_t iLink, sid_t iSessionId, const char* pCmd, size_t iLength)
{
	if (iLink < 0 || iLink >= static_cast<int>(m_vLinks.size()) || iLength == 0 || iLength > m_iMaxPendingBytes)
	{
		CloseSession(iSessionId);
		return true;
	}
	auto& stLink = m_vLinks[static_cast<size_t>(iLink)];
	if (!stLink.pAsyncContext)
	{
		::OI::Base::Bytes stError;
		AppendErr(stError, "ERR redis down");
		if (!SendBytes(iSessionId, stError))
		{
			CloseSession(iSessionId);
		}
		return true;
	}
	if (!stLink.bResuming && !stLink.vPaused.empty())
	{
		return false;
	}
	if (stLink.vPending.size() >= m_iMaxPendingCommands || iLength > m_iMaxPendingBytes - stLink.iPendingBytes)
	{
		return false;
	}
	stLink.vPending.push_back(RedisPending{iSessionId, iLength});
	stLink.iPendingBytes += iLength;
	if (redisAsyncFormattedCommand(stLink.pAsyncContext, OnReply, &stLink, pCmd, iLength) != REDIS_OK)
	{
		stLink.iPendingBytes -= iLength;
		stLink.vPending.pop_back();
		// 本地失败不能插队回包；关闭下游，已发出的旧命令仍按 FIFO 结算额度。
		CloseSession(iSessionId);
	}
	return true;
}
