#include "net_channel.h"
#include "session.h"
#include "net_os.h"

NetChannel::NetChannel(NetSocket s, Session* pSession):Channel(s, pSession)
{
	NetOs_SetNonblock(_socket.GetSocket());
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
