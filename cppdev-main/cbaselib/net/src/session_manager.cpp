
#include "config.h"
#include "logger.h"
#include "session_manager.h"
#include "session.h"
#include "accept_channel.h"
#include "connect_channel.h"
#include "tcp_channel.h"
#include "ssl_channel.h"

bool SessionManager::Init()
{
	std::string ip, port, type;
	TryGetConfigAndReturnFalse(GetIDString() + ".ip", ip);
	TryGetConfigAndReturnFalse(GetIDString() + ".port", port);
	TryGetConfigAndReturnFalse(GetIDString() + ".type", type);

	assert(GetSessionTypeByString(type) != SESSION_TYPE_INVALID);
	DefaultInit(ip, port, GetSessionTypeByString(type));
	return true;
}

bool SessionManager::StartClient(bool async)
{
	return ConnectChannel::StartConnect(this, async);

}

bool SessionManager::StartServer(bool async)
{
	return AcceptChannel::StartAccept(this, async);

}

SESSION_TYPE SessionManager::GetSessionTypeByString(const std::string& type)
{
	if (type == "tcp")
	{
		return SESSION_TYPE_TCP;
	}
	return SESSION_TYPE_INVALID;
}

SessionManager::~SessionManager()
{
	MutexGuard l(_locker);
	for (SessionMap::iterator it = _session_map.begin(); it != _session_map.end(); ++it)
	{
		Session* pSession = it->second;
		pSession->ActiveClose();
	}
	_session_map.clear();
}

const std::string SessionManager::GetIDString()
{ 
	return "SessionManager"; 
}

bool SessionManager::Send(sid_t sid, const Protocol* p)
{
	MutexGuard l(_locker);
	SessionMap::iterator it = _session_map.find(sid);
	if (it != _session_map.end())
	{
		it->second->SendProtocol(p);
		return true;
	}
	return false;
}

int SessionManager::BroadCast(const Protocol* p)
{
	MutexGuard l(_locker);
	SessionMap::iterator it = _session_map.begin();
	for (; it != _session_map.end(); ++it)
	{
		it->second->SendProtocol(p);
	}
	return _session_map.size();
}

void SessionManager::AddSession(Session* pSession)
{
	int sid = 0;
	{
		{
			MutexGuard l(_locker);
			sid = ++_cur_session_id;
			_session_map[sid] = pSession;
			pSession->SetSessionID(sid);
		}
		OnAddSession(pSession);
	}
}

void SessionManager::AddSession(const int new_fd)
{
	Session* pSession = CloneSession();
	Channel* pChannel = NULL;
	if (GetSessionType() == SESSION_TYPE_TCP)
	{
		NetSocket s;
		s.Init(new_fd, this);
		pChannel = new TCP_Channel(s, pSession);
	}
	else if (GetSessionType() == SESSION_TYPE_SSL)
	{
		NetSocket s;
		s.Init(new_fd, this);
		pChannel = new SSL_Channel(s, pSession);
	}
	
	if (pChannel)
	{
		pSession->SetChannel(pChannel);
		Poll::GetInstance()->IOEventRegister(pChannel, true, false);
		AddSession(pSession);
	}
	else
	{
		assert(false && "不支持的会话类型");
	}
}

void SessionManager::AbortSession()
{
	MutexGuard l(_locker);
	++_cur_session_id;
	OnAbortSession();
}

void SessionManager::CloseSession(sid_t sid)
{
	MutexGuard l(_locker);
	SessionMap::iterator it = _session_map.find(sid);
	if (it != _session_map.end())
	{
		Session* pSession = it->second;
		_session_map.erase(it);
		pSession->ActiveClose();
	}
}

void SessionManager::DelSession(const Session* pSession)
{
	{
		MutexGuard l(_locker);
		SessionMap::iterator it = _session_map.find(pSession->GetSessionID());
		assert(it == _session_map.end());
	}
	//已经解绑,此时有Session锁
	OnDelSession(pSession);
}