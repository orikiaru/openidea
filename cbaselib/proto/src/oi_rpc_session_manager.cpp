#include "oi_rpc_session_manager.h"
#include "oi_base.h"
#include "oi_rpc_coro.h"
namespace OI { namespace Rpc {

bool ProtoClientManager::SendRaw(const ::OI::Base::Bytes& data)
{
	sid_t sid = INVALID_SID;
	{
		::OI::Thread::SpinLockGuard l(_client_locker);
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

bool ProtoClientManager::SendRawOrPend(const ::OI::Base::Bytes& data, size_t max_pending_bytes)
{
	sid_t sid = INVALID_SID;
	{
		::OI::Thread::SpinLockGuard l(_client_locker);
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

bool ProtoClientManager::TryPendCall(uint64_t call_id, const ::OI::Base::Bytes& data)
{
	{
		::OI::Thread::SpinLockGuard l(_client_locker);
		if (_ever_connected || (_conn && _ssid != INVALID_SID))
		{
			return false;
		}
		HeldCall held;
		held.call_id = call_id;
		held.data = data;
		_held_calls.push_back(held);
	}
	MarkRpcAwaitingConnect(call_id);
	return true;
}

void ProtoClientManager::DropPendingCall(uint64_t call_id)
{
	::OI::Thread::SpinLockGuard l(_client_locker);
	for (std::deque<HeldCall>::iterator it = _held_calls.begin(); it != _held_calls.end(); ++it)
	{
		if (it->call_id == call_id)
		{
			_held_calls.erase(it);
			return;
		}
	}
}

void ProtoClientManager::FlushHeldCalls(sid_t sid)
{
	std::deque<HeldCall> held;
	{
		::OI::Thread::SpinLockGuard l(_client_locker);
		held.swap(_held_calls);
	}
	for (size_t i = 0; i < held.size(); ++i)
	{
		if (!ClaimRpcSend(held[i].call_id, sid))
		{
			continue;
		}
		if (!SendBytes(sid, held[i].data))
		{
			WakeRpcCall(held[i].call_id, OI_ERR_FAIL, NULL);
		}
	}
}

void ProtoClientManager::FailHeldCalls()
{
	std::deque<HeldCall> held;
	{
		::OI::Thread::SpinLockGuard l(_client_locker);
		held.swap(_held_calls);
		_pending.clear();
		_pending_bytes = 0;
	}
	for (size_t i = 0; i < held.size(); ++i)
	{
		WakeRpcCall(held[i].call_id, OI_ERR_CONNECT, NULL);
	}
}

void ProtoClientManager::Reconnect(int millisecond)
{	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([this]() { StartClient(); }, millisecond > 0 ? millisecond : 1);
}
}}
