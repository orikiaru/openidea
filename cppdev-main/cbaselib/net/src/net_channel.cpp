#include <unistd.h>
#include <fcntl.h>
#include "net_channel.h"
#include "session.h"

NetChannel::NetChannel(NetSocket s, Session* pSession):Channel(s, pSession)
{
	fcntl(_socket.GetSocket(), F_SETFL, (fcntl(_socket.GetSocket(), F_GETFL) | O_NONBLOCK));
	fcntl(_socket.GetSocket(), F_SETFD, FD_CLOEXEC);
}

NetChannel::~NetChannel()
{
}

bool NetChannel::TryClose()
{
	if (_p_session->IsClosed())
	{
		Poll::GetInstance()->IOEventUnRegister(this);
		Destory();
		return true;
	}
	return false;
}

void NetChannel::Destory()
{
	if (_p_session)
	{
		delete _p_session;
	}
	delete this;
}
