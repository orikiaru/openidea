#include "oi_net_channel.h"
#include "oi_session.h"
#include "oi_net_os.h"
namespace OI { namespace Net {

NetChannel::NetChannel(NetSocket s, Session* pSession):Channel(s, pSession)
{
	NetOs_SetNonblock(_socket.GetSocket());
	NetOs_SetNoDelay(_socket.GetSocket());
}

NetChannel::~NetChannel()
{
}

bool NetChannel::TryClose()
{
	if (_p_session->IsClosed())
	{
		Poll::GetInstance().IOEventUnRegister(this);
		Destory();
		return true;
	}
	return false;
}

void NetChannel::Destory()
{
	if (_p_session)
	{
		// 会话已经从管理器解绑，在销毁前通知客户端清理等待并安排重连。
		_p_session->OnClose();
		delete _p_session;
	}
	delete this;
}
}}
