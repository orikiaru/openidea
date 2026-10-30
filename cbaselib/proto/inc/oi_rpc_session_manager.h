#pragma once
#include <string>
#include <deque>
#include "oi_session_manager.h"
#include "oi_rpc_session.h"
#include "oi_rpc_call.h"
#include "oi_log.h"
namespace OI { namespace Rpc {

void FailRpcCalls(::OI::Net::SessionManager* mgr, sid_t sid);

enum CLIENT_CONNECT_MODE
{
	CLIENT_CONNECT_DIRECT = 0,
	CLIENT_CONNECT_ON_SEND = 1,
};

class ProtoSessionManager : public ::OI::Net::SessionManager
{
	void OnSessionClosed(sid_t sid) override final { FailRpcCalls(this, sid); }
public:
	ProtoSessionManager() {}
	virtual const std::string GetIDString() { return "ProtoSessionManager"; }
	virtual ::OI::Net::Session* CloneSession() { return new ProtoSession(this); }	//自定制回话类型
	virtual void OnAddSession(::OI::Net::Session* pSession)
	{
		LOG_INFO("ProtoSessionManager::OnAddSession sid=%d", pSession->GetSessionID());
	}
	virtual void OnDelSession(const ::OI::Net::Session* pSession)
	{
		LOG_INFO("ProtoSessionManager::OnDelSession sid=%d, error=%d", pSession->GetSessionID(), pSession->GetError());
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
	struct HeldCall
	{
		uint64_t call_id;
		::OI::Base::Bytes data;
	};
	std::deque<::OI::Base::Bytes> _pending;
	std::deque<HeldCall> _held_calls;
	size_t		_pending_bytes;
	bool		_ever_connected;
	::OI::Thread::SpinLock	_client_locker;
public:
	ProtoClientManager():_ssid(INVALID_SID),_conn(false),_connecting(false),_connect_mode(CLIENT_CONNECT_DIRECT),_pending_bytes(0),_ever_connected(false) {}
	void SetConnectMode(CLIENT_CONNECT_MODE mode) { _connect_mode = mode; }
	CLIENT_CONNECT_MODE GetConnectMode() const { return _connect_mode; }
	virtual void OnClientConnect(::OI::Net::Session* pSession) = 0;
	virtual void OnClientDisconnect(const ::OI::Net::Session* pSession) = 0;

	bool IsConnect()
	{
		::OI::Thread::SpinLockGuard lock(_client_locker);
		return _conn;
	}
	virtual void OnAddSession(::OI::Net::Session* pSession)
	{
		std::deque<::OI::Base::Bytes> pending;
		sid_t sid = INVALID_SID;
		{
			::OI::Thread::SpinLockGuard l(_client_locker);
			_conn = true;
			_connecting = false;
			_ssid = pSession->GetSessionID();
			sid = _ssid;
			pending.swap(_pending);
			_pending_bytes = 0;
			_ever_connected = true;
		}
		for (std::deque<::OI::Base::Bytes>::iterator it = pending.begin(); it != pending.end(); ++it)
		{
			SendBytes(sid, *it);
		}
		FlushHeldCalls(sid);
		OnClientConnect(pSession);
	}
	virtual void OnConnectFailed()
	{
		{
			::OI::Thread::SpinLockGuard l(_client_locker);
			_connecting = false;
		}
		if (_service_down || _endpoint.empty())
		{
			FailHeldCalls();
			return;
		}
		Reconnect();
	}
	virtual void OnServiceDown()
	{
		FailHeldCalls();
	}
	virtual int32_t UnavailableCode()
	{
		return IsConnect() ? OI_ERR_FAIL : OI_ERR_CONNECT;
	}
	bool TryPendCall(uint64_t call_id, const ::OI::Base::Bytes& data) override;
	void DropPendingCall(uint64_t call_id) override;
	void FlushHeldCalls(sid_t sid);
	void FailHeldCalls();
	virtual void OnDelSession(const ::OI::Net::Session* pSession)
	{
		bool reconnect = false;
		{
			::OI::Thread::SpinLockGuard l(_client_locker);
			if (_ssid == pSession->GetSessionID())
			{
				_ssid = INVALID_SID;
				_conn = false;
				reconnect = !_service_down && !_endpoint.empty() && pSession->GetEndpoint() == _endpoint;
			}
		}
		OnClientDisconnect(pSession);
		if (reconnect)
		{
			Reconnect(0);
		}
	}
	bool SendRaw(const ::OI::Base::Bytes& data);
	// 没连上先存着，连上时按顺序补发。存满 max_pending_bytes 再来的丢掉，返回 false
	bool SendRawOrPend(const ::OI::Base::Bytes& data, size_t max_pending_bytes);
	void Reconnect(int delay_ms = 2000);

};
}}
