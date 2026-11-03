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
		::OI::Thread::SpinLockGuard lock(_client_locker);
		if (_ever_connected || (_conn && _ssid != INVALID_SID)) return false;
		if (_held_calls.size() < _held_max_count && _held_bytes <= _held_max_bytes
			&& data.Size() <= _held_max_bytes - _held_bytes)
		{
			// 先标记再入队；超时回调会在本锁释放后删除。已经超时则不再留副本。
			if (!MarkRpcAwaitingConnect(call_id)) return true;
			_held_calls.push_back(HeldCall{call_id, data});
			_held_index.emplace(call_id, std::prev(_held_calls.end()));
			_held_bytes += data.Size();
			return true;
		}
	}
	// 不持客户端锁唤醒，回调允许再次发起请求。
	WakeRpcCall(call_id, OI_ERR_BUSY, nullptr);
	return true;
}

void ProtoClientManager::DropPendingCall(uint64_t call_id)
{
	::OI::Thread::SpinLockGuard lock(_client_locker);
	auto it = _held_index.find(call_id);
	if (it == _held_index.end()) return;
	_held_bytes -= it->second->data.Size();
	_held_calls.erase(it->second);
	_held_index.erase(it);
}

void ProtoClientManager::FlushHeldCalls(sid_t sid)
{
	std::list<HeldCall> held;
	{
		::OI::Thread::SpinLockGuard l(_client_locker);
		held.swap(_held_calls);
		_held_index.clear();
		_held_bytes = 0;
	}
	for (const auto& call : held)
	{
		if (!ClaimRpcSend(call.call_id, sid))
		{
			continue;
		}
		if (!SendBytes(sid, call.data))
		{
			WakeRpcCall(call.call_id, OI_ERR_FAIL, NULL);
		}
	}
}

void ProtoClientManager::FailHeldCalls()
{
	std::list<HeldCall> held;
	{
		::OI::Thread::SpinLockGuard l(_client_locker);
		held.swap(_held_calls);
		_held_index.clear();
		_held_bytes = 0;
		_pending.clear();
		_pending_bytes = 0;
	}
	for (const auto& call : held)
	{
		WakeRpcCall(call.call_id, OI_ERR_CONNECT, NULL);
	}
}

void ProtoClientManager::Reconnect(int millisecond)
{	::OI::Timer::BaseTimerManager::GetInstance().AddTimer([this]() { StartClient(); }, millisecond > 0 ? millisecond : 1);
}
}}
