#include "oi_rpc_session_manager.h"
#include "oi_parser.h"
#include "oi_base.h"

bool ProtoClientManager::SendRaw(const OI::Base::Bytes& data)
{
	sid_t sid = INVALID_SID;
	{
		OI::Thread::SpinLockGuard l(_client_locker);
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

bool ProtoClientManager::SendRawOrPend(const OI::Base::Bytes& data, size_t max_pending_bytes)
{
	sid_t sid = INVALID_SID;
	{
		OI::Thread::SpinLockGuard l(_client_locker);
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
		OI::Thread::SpinLockGuard l(_client_locker);
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
	OI::Base::BytesStream encoded = parser.EncodeProtocol(p);
	bool need_connect = false;
	{
		OI::Thread::SpinLockGuard l(_client_locker);
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
		OI::Thread::SpinLockGuard l(_client_locker);
		_connecting = false;
		return false;
	}
	return true;
}

void ProtoClientManager::Reconnect(int millisecond)
{
	BaseTimerManager::GetInstance().AddTimer([this]() { StartClient(); }, millisecond);
}
