
#include "oi_config.h"
#include "oi_log.h"
#include "oi_registry.h"
#include "oi_session_manager.h"
#include "oi_session.h"
#include "oi_accept_channel.h"
#include "oi_connect_channel.h"
#include "oi_tcp_channel.h"
#include <stdio.h>
#include <vector>
namespace OI { namespace Net {

bool SessionManager::Init()
{
	std::string ip, port, type;
	TryGetConfigAndReturnFalse(GetIDString() + ".ip", ip);
	TryGetConfigAndReturnFalse(GetIDString() + ".port", port);
	TryGetConfigAndReturnFalse(GetIDString() + ".type", type);

	std::string service;
	if (GetIDString() != "Registry"
		&& ::OI::Base::Config::GlobalConfig().GetAs(GetIDString() + ".service", service)
		&& !service.empty())
	{
		std::string found_ip;
		std::string found_port;
		if (::OI::Base::RegistryClient::GetInstance().Lookup(service, found_ip, found_port))
		{
			ip = found_ip;
			port = found_port;
		}
		_service_name = service;
	}

	assert(GetSessionTypeByString(type) != SESSION_TYPE_INVALID);
	DefaultInit(ip, port, GetSessionTypeByString(type));
	_endpoint = ip + ":" + port;
	if (!_service_name.empty())
	{
		::OI::Base::RegistryClient::GetInstance().Watch(this, _service_name);
	}
	return true;
}

bool SessionManager::StartClient(bool async)
{	if (_service_down)
	{
		return false;
	}
	return ConnectChannel::StartConnect(this, async);

}

void SessionManager::CloseEndpoint(const std::string& endpoint)
{
	if (endpoint.empty())
	{
		return;
	}
	std::vector<sid_t> ids;
	{
		::OI::Thread::MutexGuard lock(_locker);
		for (SessionMap::iterator it = _session_map.begin(); it != _session_map.end(); ++it)
		{
			if (it->second->GetEndpoint() == endpoint)
			{
				ids.push_back(it->first);
			}
		}
	}
	for (size_t i = 0; i < ids.size(); ++i)
	{
		CloseSession(ids[i]);
	}
}

bool SessionManager::HasEndpoint(const std::string& endpoint)
{
	::OI::Thread::MutexGuard lock(_locker);
	for (SessionMap::iterator it = _session_map.begin(); it != _session_map.end(); ++it)
	{
		if (it->second->GetEndpoint() == endpoint)
		{
			return true;
		}
	}
	return false;
}

void SessionManager::CloseAllSessions()
{
	std::vector<sid_t> ids;
	{
		::OI::Thread::MutexGuard lock(_locker);
		for (SessionMap::iterator it = _session_map.begin(); it != _session_map.end(); ++it)
		{
			ids.push_back(it->first);
		}
	}
	for (size_t i = 0; i < ids.size(); ++i)
	{
		CloseSession(ids[i]);
	}
}

void SessionManager::ApplyService(const std::string& ip, const std::string& port, bool online)
{
	if (_service_name.empty())
	{
		return;
	}
	if (!online)
	{
		if (!_service_seen || _service_down)
		{
			return;
		}
		_service_down = true;
		std::string previous = _endpoint;
		_endpoint.clear();
		LOG_TRACE("service offline %s", _service_name.c_str());
		fflush(stdout);
		CloseEndpoint(previous);
		OnServiceDown();
		return;
	}
	std::string endpoint = ip + ":" + port;
	bool was_down = _service_down;
	bool moved = _endpoint != endpoint;
	_service_seen = true;
	if (!was_down && !moved)
	{
		return;
	}
	std::string previous = _endpoint;
	_service_down = false;
	_endpoint = endpoint;
	DefaultInit(ip, port, _session_type);
	LOG_TRACE("service switch %s %s -> %s", _service_name.c_str(), previous.c_str(), endpoint.c_str());
	fflush(stdout);
	if (!previous.empty() && previous != endpoint)
	{
		CloseEndpoint(previous);
	}
	if (!HasEndpoint(endpoint))
	{
		StartClient();
	}
}

bool SessionManager::StartServer(bool async)
{
	bool ok = AcceptChannel::StartAccept(this, async);
	if (ok)
	{
		::OI::Base::RegistryClient::GetInstance().Publish(GetIDString(), GetStrIP(), GetStrPort());
	}
	return ok;
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

void SessionManager::AddSession(Session* pSession)
{
	int sid = 0;
	{
		{
			::OI::Thread::MutexGuard l(_locker);
			sid = ++_cur_session_id;
			_session_map[sid] = pSession;
			pSession->SetSessionID(sid);
			pSession->SetEndpoint(_sip + ":" + _sport);
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
	{
		::OI::Thread::MutexGuard l(_locker);
		++_cur_session_id;
	}
	LOG_ERROR("SessionManager::AbortSession id=%s endpoint=%s", GetIDString().c_str(), _endpoint.c_str());	OnConnectFailed();
}

void SessionManager::CloseSession(sid_t sid)
{
	bool closed = false;
	{
		::OI::Thread::MutexGuard l(_locker);
		SessionMap::iterator it = _session_map.find(sid);
		if (it != _session_map.end())
		{
			Session* pSession = it->second;
			_session_map.erase(it);
			pSession->ActiveClose();
			closed = true;
		}
	}
	// 逻辑断开立即结束等待；唤醒放在管理器锁外。
	if (closed) OnSessionClosed(sid);
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
