#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "oi_accept_channel.h"
#include "oi_session_manager.h"
#include "oi_poll.h"
#include "oi_net_os.h"
#include "oi_log.h"
namespace OI { namespace Net {

void AcceptChannel::DataIn()
{
	for (;;)
	{
		int accept_count = _socket.SocketAcceptOne(_p_session_mgr);
		if (accept_count == -1)
		{
			// 描述符用尽时连接还在队列里，监听口一直可读。先停一会儿，不让 poll 线程空转。
			Poll::GetInstance().PauseEvents(this, 100);
			break;
		}
		if (accept_count == 0)
		{
			break;
		}
	}
}

void AcceptChannel::DataOut()
{
	DataIn();
}

bool AcceptChannel::TryClose()
{
	return false;
}

bool AcceptChannel::StartAccept(SessionManager* pMgr, bool async)
{
	int session_type = pMgr->GetSessionType();
	switch (session_type)
	{
	case SESSION_TYPE_TCP:
		return StartTcpAccept(pMgr, async);
	default:
		printf("SessionManager::StartServer not support session_type=%d\n", session_type);
		return false;
	}

}

bool AcceptChannel::StartTcpAccept(SessionManager* pMgr, bool async)
{
	// 只支持异步监听。同步 accept 会把调用线程永久堵住。
	if (!async)
	{
		LOG_ERROR("AcceptChannel::StartTcpAccept sync accept not supported id=%s", pMgr->GetIDString().c_str());
		return false;
	}
	NetSocket s;
	if (!s.CreateSocket(pMgr, async))
	{
		return false;
	}

	if (!s.SocketListenAndBind(pMgr))
	{
		NetOs_Close(s.GetSocket());
		s.Detach();
		return false;
	}

	AcceptChannel* pChannel = new AcceptChannel(s, pMgr);
	Poll::GetInstance().IOEventRegister(pChannel, true, true);
	return true;
}
}}

