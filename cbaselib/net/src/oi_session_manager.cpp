
#include "oi_config.h"
#include "oi_log.h"
#include "oi_session_manager.h"
#include "oi_session.h"
#include "oi_accept_channel.h"
#include "oi_connect_channel.h"
#include "oi_tcp_channel.h"
namespace OI { namespace Net {

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
	::OI::Thread::MutexGuard l(_locker);
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

bool SessionManager::Send(sid_t sid, const ::OI::Rpc::Protocol* p)
{
	::OI::Thread::MutexGuard l(_locker);
	SessionMap::iterator it = _session_map.find(sid);
	if (it != _session_map.end())
	{
		it->second->SendProtocol(p);
		return true;
	}
	return false;
}

bool SessionManager::SendBytes(sid_t sid, const ::OI::Base::Bytes& data)
{
	::OI::Thread::MutexGuard l(_locker);
	SessionMap::iterator it = _session_map.find(sid);
	if (it != _session_map.end())
	{
		it->second->Send(data);
		return true;
	}
	return false;
}

int SessionManager::BroadCast(const ::OI::Rpc::Protocol* p)
{
	::OI::Thread::MutexGuard l(_locker);
	SessionMap::iterator it = _session_map.begin();
	for (; it != _session_map.end(); ++it)
	{
		it->second->SendProtocol(p);
	}
	return (int)_session_map.size();
}

void SessionManager::AddSession(Session* pSession)
{
	int sid = 0;
	{
		{
			::OI::Thread::MutexGuard l(_locker);
			sid = ++_cur_session_id;
			_session_map[sid] = pSession;
			pSession->SetSessionID(sid);
		}
		OnAddSession(pSession);
	}
}

void SessionManager::AddSession(net_socket_t new_fd)
{
	Session* pSession = CloneSession();
	Channel* pChannel = NULL;
	if (GetSessionType() == SESSION_TYPE_TCP)
	{
		NetSocket s;
		s.Init(new_fd, this);
		pChannel = new TCP_Channel(s, pSession);
	}

	if (pChannel)
	{
		pSession->SetChannel(pChannel);
		Poll::GetInstance().IOEventRegister(pChannel, true, false);
		AddSession(pSession);
	}
	else
	{
		assert(false && "不支持的会话类型");
	}
}

void SessionManager::AbortSession()
{
	::OI::Thread::MutexGuard l(_locker);
	++_cur_session_id;
	OnConnectFailed();
	OnAbortSession();
}

void SessionManager::CloseSession(sid_t sid)
{
	::OI::Thread::MutexGuard l(_locker);
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
		::OI::Thread::MutexGuard l(_locker);
		SessionMap::iterator it = _session_map.find(pSession->GetSessionID());
		assert(it == _session_map.end());
	}
	//已经解绑,此时有Session锁
	OnDelSession(pSession);
}
}}