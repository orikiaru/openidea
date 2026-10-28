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
	redisAsyncContext* _ac;
	int32_t _mask;
	bool _in_io;
	bool _dead;
public:
	RedisEv(::OI::Net::NetSocket sock, redisAsyncContext* ac)
		: ::OI::Net::Channel(sock, NULL), _ac(ac), _mask(0), _in_io(false), _dead(false)
	{
	}
	void DataIn()
	{
		_in_io = true;
		if (_ac)
		{
			redisAsyncHandleRead(_ac);
		}
	}
	void DataOut()
	{
		_in_io = true;
		if (_ac)
		{
			redisAsyncHandleWrite(_ac);
		}
	}
	bool TryClose()
	{
		if (!_dead)
		{
			_in_io = false;
			return false;
		}
		::OI::Net::Poll::GetInstance().IOEventUnRegister(this, false);
		delete this;
		return true;
	}
	void Add(int32_t bit)
	{
		_mask |= bit;
		Touch();
	}
	void Del(int32_t bit)
	{
		_mask &= ~bit;
		Touch();
	}
	void Touch()
	{
		if (_dead)
		{
			return;
		}
		if (_poll_token == NULL)
		{
			if (_mask == 0)
			{
				return;
			}
			::OI::Net::Poll::GetInstance().IOEventRegister(this,
				(_mask & ::OI::Net::IO_EV_READ) != 0, (_mask & ::OI::Net::IO_EV_WRITE) != 0);
			return;
		}
		::OI::Net::Poll::GetInstance().IOEventMod(this, _mask);
	}
	void Cleanup()
	{
		_ac = NULL;
		_dead = true;
		if (_in_io)
		{
			return;
		}
		::OI::Net::Poll::GetInstance().IOEventUnRegister(this, false);
		delete this;
	}
};

void EvAddRead(void* p) { ((RedisEv*)p)->Add(::OI::Net::IO_EV_READ); }
void EvDelRead(void* p) { ((RedisEv*)p)->Del(::OI::Net::IO_EV_READ); }
void EvAddWrite(void* p) { ((RedisEv*)p)->Add(::OI::Net::IO_EV_WRITE); }
void EvDelWrite(void* p) { ((RedisEv*)p)->Del(::OI::Net::IO_EV_WRITE); }
void EvCleanup(void* p) { ((RedisEv*)p)->Cleanup(); }

int AttachRedis(redisAsyncContext* ac)
{
	if (!ac || ac->ev.data != NULL)
	{
		return REDIS_ERR;
	}
	::OI::Net::NetSocket sock;
	sock.Attach(ac->c.fd);
	RedisEv* ev = new RedisEv(sock, ac);
	ac->ev.addRead = EvAddRead;
	ac->ev.delRead = EvDelRead;
	ac->ev.addWrite = EvAddWrite;
	ac->ev.delWrite = EvDelWrite;
	ac->ev.cleanup = EvCleanup;
	ac->ev.data = ev;
	return REDIS_OK;
}

void AppendBulk(::OI::Base::Bytes& out, const char* s, size_t n)
{
	char hdr[64];
	int hn = snprintf(hdr, sizeof(hdr), "$%zu\r\n", n);
	out.Append(hdr, (size_t)hn);
	if (n && s)
	{
		out.Append(s, n);
	}
	out.Append("\r\n", 2);
}

void AppendReply(::OI::Base::Bytes& out, const redisReply* reply)
{
	if (!reply)
	{
		out.Append("$-1\r\n", 5);
		return;
	}
	char hdr[64];
	int hn = 0;
	switch (reply->type)
	{
	case REDIS_REPLY_STATUS:
		out.Append("+", 1);
		if (reply->str)
		{
			out.Append(reply->str, reply->len);
		}
		out.Append("\r\n", 2);
		break;
	case REDIS_REPLY_ERROR:
		out.Append("-", 1);
		if (reply->str)
		{
			out.Append(reply->str, reply->len);
		}
		out.Append("\r\n", 2);
		break;
	case REDIS_REPLY_INTEGER:
	case REDIS_REPLY_BOOL:
		hn = snprintf(hdr, sizeof(hdr), ":%lld\r\n", reply->integer);
		out.Append(hdr, (size_t)hn);
		break;
	case REDIS_REPLY_NIL:
		out.Append("$-1\r\n", 5);
		break;
	case REDIS_REPLY_STRING:
		AppendBulk(out, reply->str, reply->len);
		break;
	case REDIS_REPLY_ARRAY:
		hn = snprintf(hdr, sizeof(hdr), "*%zu\r\n", reply->elements);
		out.Append(hdr, (size_t)hn);
		for (size_t i = 0; i < reply->elements; ++i)
		{
			AppendReply(out, reply->element[i]);
		}
		break;
	default:
		out.Append("-ERR reply\r\n", 12);
		break;
	}
}

void AppendErr(::OI::Base::Bytes& out, const char* text)
{
	out.Append("-", 1);
	out.Append(text, strlen(text));
	out.Append("\r\n", 2);
}

void OnReply(redisAsyncContext*, void* reply, void* priv)
{
	sid_t sid = (sid_t)(intptr_t)priv;
	// 单线程进程，回包缓冲复用一块，不必每条回执分配。
	static ::OI::Base::Bytes out;
	out.Clear();
	if (!reply)
	{
		AppendErr(out, "ERR redis down");
	}
	else
	{
		AppendReply(out, (redisReply*)reply);
		freeReplyObject(reply);
	}
	RedisServer::GetInstance().SendBytes(sid, out);
}

void OnConnect(redisAsyncContext* c, int status)
{
	RedisLink* link = (RedisLink*)c->data;
	if (status != REDIS_OK)
	{
		LOG_ERROR("RedisServer hiredis connect %s", c->c.errstr);
		link->ac = NULL;
		RedisServer::GetInstance().ArmReconnect(link);
		return;
	}
	LOG_TRACE("RedisServer hiredis up index=%d", link->index);
}

void OnDisconnect(const redisAsyncContext* c, int status)
{
	(void)status;
	RedisLink* link = (RedisLink*)c->data;
	LOG_TRACE("RedisServer hiredis down index=%d", link->index);
	link->ac = NULL;
	RedisServer::GetInstance().ArmReconnect(link);
}
}

RedisServer::RedisServer() : _connections(1), _next(0)
{
}

RedisServer::~RedisServer()
{
	for (size_t i = 0; i < _links.size(); ++i)
	{
		if (_links[i].ac)
		{
			redisAsyncFree(_links[i].ac);
			_links[i].ac = NULL;
		}
	}
}

bool RedisServer::Init()
{
	if (!::OI::Net::SessionManager::Init())
	{
		return false;
	}
	if (!TryGetConfig("Redis.ip", _redis_ip) || !TryGetConfig("Redis.port", _redis_port))
	{
		LOG_ERROR("RedisServer missing Redis.ip or Redis.port");
		return false;
	}
	_connections = 1;
	TryGetConfig("Redis.connections", _connections);
	if (_connections < 1)
	{
		_connections = 1;
	}
	_links.resize((size_t)_connections);
	for (int i = 0; i < _connections; ++i)
	{
		_links[(size_t)i].index = i;
		Open(&_links[(size_t)i]);
	}
	LOG_TRACE("RedisServer connections=%d", _connections);
	return true;
}

::OI::Net::Session* RedisServer::CloneSession()
{
	return new RedisServerSession(this);
}

void RedisServer::OnAddSession(::OI::Net::Session* pSession)
{
	int n = 0;
	if (!_links.empty())
	{
		n = _next % (int)_links.size();
		++_next;
	}
	((RedisServerSession*)pSession)->Bind(n);
	LOG_TRACE("RedisServer::OnAddSession sid=%d link=%d", pSession->GetSessionID(), n);
}

void RedisServer::OnDelSession(const ::OI::Net::Session* pSession)
{
	LOG_TRACE("RedisServer::OnDelSession sid=%d", pSession->GetSessionID());
}

void RedisServer::Open(RedisLink* link)
{
	if (link->ac)
	{
		return;
	}
	redisAsyncContext* ac = redisAsyncConnect(_redis_ip.c_str(), std::atoi(_redis_port.c_str()));
	if (!ac)
	{
		ArmReconnect(link);
		return;
	}
	ac->data = link;
	ac->c.flags |= REDIS_NO_AUTO_FREE_REPLIES;
	if (ac->err)
	{
		LOG_ERROR("RedisServer hiredis %s", ac->c.errstr);
		redisAsyncFree(ac);
		ArmReconnect(link);
		return;
	}
	if (AttachRedis(ac) != REDIS_OK)
	{
		LOG_ERROR("RedisServer hiredis attach");
		redisAsyncFree(ac);
		ArmReconnect(link);
		return;
	}
	link->ac = ac;
	redisAsyncSetConnectCallbackNC(ac, OnConnect);
	redisAsyncSetDisconnectCallback(ac, OnDisconnect);
}

void RedisServer::ArmReconnect(RedisLink* link)
{
	if (link->dialing)
	{
		return;
	}
	link->dialing = true;
	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([link]() {
		link->dialing = false;
		RedisServer::GetInstance().Open(link);
	}, 2000);
}

void RedisServer::Issue(int32_t link, sid_t sid, const char* cmd, size_t len)
{
	::OI::Base::Bytes err;
	if (link < 0 || link >= (int)_links.size() || !_links[(size_t)link].ac || len == 0)
	{
		AppendErr(err, "ERR redis down");
		SendBytes(sid, err);
		return;
	}
	if (redisAsyncFormattedCommand(_links[(size_t)link].ac, OnReply, (void*)(intptr_t)sid, cmd, len) != REDIS_OK)
	{
		AppendErr(err, "ERR redis down");
		SendBytes(sid, err);
	}
}
