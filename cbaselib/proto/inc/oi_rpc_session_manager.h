#pragma once
#include <string>
#include <deque>
#include "oi_session_manager.h"
#include "oi_rpc_session.h"
#include "oi_log.h"
namespace OI { namespace Rpc {

enum CLIENT_CONNECT_MODE
{
	CLIENT_CONNECT_DIRECT = 0,
	CLIENT_CONNECT_ON_SEND = 1,
};

class ProtoSessionManager : public ::OI::Net::SessionManager
{
public:
	ProtoSessionManager() {}
	virtual const std::string GetIDString() { return "ProtoSessionManager"; }
	virtual ::OI::Net::Session* CloneSession() { return new ProtoSession(this); }	//自定制回话类型
	virtual void OnAddSession(::OI::Net::Session* pSession)
	{
		LOG_TRACE("ProtoSessionManager::OnAddSession sid=%d", pSession->GetSessionID());
	}
	virtual void OnDelSession(const ::OI::Net::Session* pSession)
	{
		LOG_TRACE("ProtoSessionManager::OnDelSession sid=%d, error=%d", pSession->GetSessionID(), pSession->GetError());
	}
	virtual void OnAbortSession()
	{
		LOG_TRACE("ProtoSessionManager::OnAbortSessionsid sid=%d", _cur_session_id);
	}
};

//client只负责连接一个server
//如果需要连接池,需要建立多个client,自行设计吧
class ProtoClientManager : public ProtoSessionManager
{
	sid_t		_ssid;
	bool		_conn;
	bool		_connecting;
	CLIENT_CONNECT_MODE _connect_mode;
	std::deque<::OI::Base::Bytes> _pending;
	size_t		_pending_bytes;
	::OI::Thread::SpinLock	_client_locker;
public:
	ProtoClientManager():_ssid(INVALID_SID),_conn(false),_connecting(false),_connect_mode(CLIENT_CONNECT_DIRECT),_pending_bytes(0) {}
	void SetConnectMode(CLIENT_CONNECT_MODE mode) { _connect_mode = mode; }
	CLIENT_CONNECT_MODE GetConnectMode() const { return _connect_mode; }
	virtual void OnClientConnect(::OI::Net::Session* pSession) = 0;
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession) = 0;

	bool IsConnect() { return _conn; }
	virtual void OnAddSession(::OI::Net::Session* pSession)
	{
		std::deque<::OI::Base::Bytes> pending;
		sid_t sid = INVALID_SID;
		{
			::OI::Thread::SpinLockGuard l(_client_locker);
			assert(_ssid == INVALID_SID);
			_conn = true;
			_connecting = false;
			_ssid = pSession->GetSessionID();
			sid = _ssid;
			pending.swap(_pending);
			_pending_bytes = 0;
		}
		for (std::deque<::OI::Base::Bytes>::iterator it = pending.begin(); it != pending.end(); ++it)
		{
			SendBytes(sid, *it);
		}
		OnClientConnect(pSession);
	}
	virtual void OnConnectFailed()
	{
		::OI::Thread::SpinLockGuard l(_client_locker);
		_connecting = false;
	}
	virtual void OnDelSession(const ::OI::Net::Session* pSession)
	{
		{
			::OI::Thread::SpinLockGuard l(_client_locker);
			_ssid = INVALID_SID;
			_conn = false;
		}
		OnClientDisconnect(pSession);
	}
	virtual void OnAbortSession() = 0;	//客户端必须重写
	bool SendProtocol(const Protocol& p) { return SendProtocol(&p); }
	bool SendProtocol(const Protocol* p);
	bool SendRaw(const ::OI::Base::Bytes& data);
	// 没连上先存着，连上时按顺序补发。存满 max_pending_bytes 再来的丢掉，返回 false
	bool SendRawOrPend(const ::OI::Base::Bytes& data, size_t max_pending_bytes);
	void Reconnect(int delay_ms = 2000);

};
}}
