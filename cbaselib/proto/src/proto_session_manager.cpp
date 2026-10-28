#include "proto_session_manager.h"
#include "timer_task.h"
#include "parser.h"
#include "base_header.h"

bool ProtoClientManager::SendRaw(const Bytes& data)
{
	sid_t sid = INVALID_SID;
	{
		SpinLockGuard l(_client_locker);
		if (_conn && _ssid != INVALID_SID)
		{
			sid = _ssid;
		}
	}
	if (sid == INVALID_SID)
	{
		return false;
	}
	return SendBytes(sid, data);
}

bool ProtoClientManager::SendRawOrPend(const Bytes& data, size_t max_pending_bytes)
{
	sid_t sid = INVALID_SID;
	{
		SpinLockGuard l(_client_locker);
		if (_conn && _ssid != INVALID_SID)
		{
			sid = _ssid;
		}
		else
		{
			if (_pending_bytes + data.Size() > max_pending_bytes)
			{
				return false;
			}
			_pending.push_back(data);
			_pending_bytes += data.Size();
			return true;
		}
	}
	return SendBytes(sid, data);
}

bool ProtoClientManager::SendProtocol(const Protocol* p)
{
	if (!p)
	{
		return false;
	}

	sid_t sid = INVALID_SID;
	{
		SpinLockGuard l(_client_locker);
		if (_conn && _ssid != INVALID_SID)
		{
			sid = _ssid;
		}
	}
	if (sid != INVALID_SID)
	{
		return Send(sid, p);
	}
	if (_connect_mode != CLIENT_CONNECT_ON_SEND)
	{
		return false;
	}

	ProtocolParser parser;
	BytesStream encoded = parser.EncodeProtocol(p);
	bool need_connect = false;
	{
		SpinLockGuard l(_client_locker);
		if (_conn && _ssid != INVALID_SID)
		{
			sid = _ssid;
		}
		else
		{
			_pending.push_back(encoded);
			_pending_bytes += encoded.Size();
			if (!_connecting)
			{
				_connecting = true;
				need_connect = true;
			}
		}
	}
	if (sid != INVALID_SID)
	{
		return Send(sid, p);
	}
	if (need_connect && !StartClient())
	{
		SpinLockGuard l(_client_locker);
		_connecting = false;
		return false;
	}
	return true;
}

void ProtoClientManager::Reconnect(int millisecond)
{
	class ReconnTask : public TimerTask
	{
	public:
		ProtoClientManager* p_mgr;
		ReconnTask(ProtoClientManager* pMgr) :p_mgr(pMgr) {}
		virtual void Handle()
		{
			p_mgr->StartClient();
		}
	};

	BaseTimerManager::GetInstance()->AddTimer(new ReconnTask(this), millisecond);
}
