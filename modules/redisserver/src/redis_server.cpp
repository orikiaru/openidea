#include "redis_server.h"
#include "redis_server_session.h"
#include "hiredis.h"
#include "async.h"
#include "oi_channel.h"
#include "oi_poll.h"
#include "oi_socket.h"
#include "oi_timer_manager.h"
#include "oi_config.h"
#include "oi_log.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

void EvAddRead(void* pPointerParam) { ((RedisEv*)pPointerParam)->Add(::OI::Net::IO_EV_READ); }
void EvDelRead(void* pPointerParam) { ((RedisEv*)pPointerParam)->Del(::OI::Net::IO_EV_READ); }
void EvAddWrite(void* pPointerParam) { ((RedisEv*)pPointerParam)->Add(::OI::Net::IO_EV_WRITE); }
void EvDelWrite(void* pPointerParam) { ((RedisEv*)pPointerParam)->Del(::OI::Net::IO_EV_WRITE); }
void EvCleanup(void* pPointerParam) { ((RedisEv*)pPointerParam)->Cleanup(); }

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
	sid_t iSessionIdLocal = (sid_t)(intptr_t)pPrivParam;
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
	if (!RedisServer::GetInstance().SendBytes(iSessionIdLocal, stOutput)) RedisServer::GetInstance().CloseSession(iSessionIdLocal);
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

RedisServer::RedisServer() : m_iConnections(1), m_iNext(0)
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
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([pLinkParam]() {
		pLinkParam->bDialing = false;
		RedisServer::GetInstance().Open(pLinkParam);
	}, 2000);
}

void RedisServer::Issue(int32_t iLinkParam, sid_t iSessionIdParam, const char* pCmdParam, size_t iLengthParam)
{
	::OI::Base::Bytes stErrorLocal;
	if (iLinkParam < 0 || iLinkParam >= (int)m_vLinks.size() || !m_vLinks[(size_t)iLinkParam].pAsyncContext || iLengthParam == 0)
	{
		AppendErr(stErrorLocal, "ERR redis down");
		if (!SendBytes(iSessionIdParam, stErrorLocal)) CloseSession(iSessionIdParam);
		return;
	}
	if (redisAsyncFormattedCommand(m_vLinks[(size_t)iLinkParam].pAsyncContext, OnReply, (void*)(intptr_t)iSessionIdParam, pCmdParam, iLengthParam) != REDIS_OK)
	{
		AppendErr(stErrorLocal, "ERR redis down");
		if (!SendBytes(iSessionIdParam, stErrorLocal)) CloseSession(iSessionIdParam);
	}
}
