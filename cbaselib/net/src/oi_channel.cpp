#include "oi_channel.h"
#include "oi_session.h"
#include "oi_poll.h"
#include "oi_net_os.h"
namespace OI { namespace Net {

Channel::~Channel()
{
	if (_poll_token)
	{
		Poll::GetInstance().IOEventUnRegister(this);
	}
	NetOs_Close(_socket.GetSocket());
	_socket.Detach();
}

void Channel::AllowSend()
{
	::OI::Thread::MutexGuard l(_locker);
	// 关闭中的会话也要触发一次 IO，让 TryClose 回收空闲连接，不能等对端发数据。
	if (_p_session && (0 == (_event_mask & IO_EV_WRITE)))
	{
		_event_mask |= IO_EV_WRITE;
		Poll::GetInstance().IOEventMod(this, _event_mask);
	}
}

void Channel::ForbidSend()
{
	::OI::Thread::MutexGuard l(_locker);
	if (_p_session && !_p_session->IsClosed() && (_event_mask & IO_EV_WRITE))
	{
		_event_mask &= ~IO_EV_WRITE;
		Poll::GetInstance().IOEventMod(this, _event_mask);
	}
}

void Channel::AllowRead()
{
	::OI::Thread::MutexGuard l(_locker);
	if (_p_session && !_p_session->IsClosed() && (0 == (_event_mask & IO_EV_READ)))
	{
		_event_mask |= IO_EV_READ;
		Poll::GetInstance().IOEventMod(this, _event_mask);
	}
}

void Channel::ForbidRead()
{
	::OI::Thread::MutexGuard l(_locker);
	if (_p_session && !_p_session->IsClosed() && (_event_mask & IO_EV_READ))
	{
		_event_mask &= ~IO_EV_READ;
		Poll::GetInstance().IOEventMod(this, _event_mask);
	}
}
}}
