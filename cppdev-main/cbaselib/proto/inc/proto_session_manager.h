#pragma once
#include <string>
#include "session_manager.h"
#include "proto_session.h"
#include "logger.h"

class ProtoSessionManager : public SessionManager
{
public:
	ProtoSessionManager() {}
	virtual const std::string GetIDString() { return "ProtoSessionManager"; }
	virtual Session* CloneSession() { return new ProtoSession(this); }	//自定制回话类型
	virtual void OnAddSession(Session* pSession)
	{
		LOG_TRACE("ProtoSessionManager::OnAddSession sid=%d", pSession->GetSessionID());
	}
	virtual void OnDelSession(const Session* pSession)
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
	SpinLock	_client_locker;
public:
	ProtoClientManager():_ssid(INVALID_SID),_conn(false) {}
	virtual void OnClientConnect(Session* pSession) = 0;
	virtual void OnClientDisconnect(const Session* pSession) = 0;

	bool IsConnect() { return _conn; }
	virtual void OnAddSession(Session* pSession)
	{
		{
			SpinLockGuard l(_client_locker);
			assert(_ssid == INVALID_SID);
			_conn = true;
			_ssid = pSession->GetSessionID();
		}
		//多连接直接嘎掉
		OnClientConnect(pSession);
	}
	virtual void OnDelSession(const Session* pSession)
	{
		{
			SpinLockGuard l(_client_locker);
			_ssid = INVALID_SID;
			_conn = false;
		}
		OnClientDisconnect(pSession);
	}
	virtual void OnAbortSession() = 0;	//客户端必须重写
	bool SendProtocol(const Protocol& p) { return SendProtocol(&p); }
	bool SendProtocol(const Protocol* p);
	void Reconnect(int delay_ms = 2000);

};
