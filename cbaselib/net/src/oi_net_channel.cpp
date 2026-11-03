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
		// 发送方可能还持着会话。先断开，它拿到会话锁后就看不到这个通道，不会再改已注销的事件。
		_p_session->DetachChannel();
		Poll::GetInstance().IOEventUnRegister(this);
		Destroy();
		return true;
	}
	return false;
}

void NetChannel::Destroy()
{
	if (_p_session)
	{
		// 会话已经从管理器解绑，在销毁前通知客户端清理等待并安排重连。
		_p_session->OnClose();
		_p_session->Release();
	}
	delete this;
}
}}
