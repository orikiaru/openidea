#include <fcntl.h>
#include <unistd.h>
#include "channel.h"
#include "session.h"

Channel::~Channel()
{
	close(_socket.GetSocket());
}

void Channel::AllowSend()
{
	MutexGuard l(_locker);
	if (!_p_session->IsClosed() && (0 == (_event_mask & EPOLLOUT)))
	{
		_event_mask |= EPOLLOUT;
		Poll::GetInstance()->IOEventMod(this, _event_mask);
	}
}

void Channel::ForbidSend()
{
	MutexGuard l(_locker);
	if (!_p_session->IsClosed() && (_event_mask & EPOLLOUT))
	{
		_event_mask &= ~EPOLLOUT;
		Poll::GetInstance()->IOEventMod(this, _event_mask);
	}
}

void Channel::AllowRead()
{
	MutexGuard l(_locker);
	if (!_p_session->IsClosed() && (0 == (_event_mask & EPOLLIN)))
	{
		_event_mask |= EPOLLIN;
		Poll::GetInstance()->IOEventMod(this, _event_mask);
	}
}

void Channel::ForbidRead()
{
	MutexGuard l(_locker);
	if (!_p_session->IsClosed() && (_event_mask & EPOLLIN))
	{
		_event_mask &= ~EPOLLIN;
		Poll::GetInstance()->IOEventMod(this, _event_mask);
	}
}