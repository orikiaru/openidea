#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "oi_accept_channel.h"
#include "oi_session_manager.h"
#include "oi_log.h"
namespace OI { namespace Net {

void AcceptChannel::DataIn()
{
	for (;;)
	{
		int accept_count = _socket.SocketAcceptOne(_p_session_mgr);
		if (accept_count == -1)
		{
			assert(false);
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
	//当同步调用时,线程会被永久性阻塞
	NetSocket s;
	if (!s.CreateSocket(pMgr, async))
	{
		return false;
	}

	if (!s.SocketListenAndBind(pMgr))
	{
		return false;
	}

	if (s.IsAsync())
	{
		AcceptChannel* pChannel = new AcceptChannel(s, pMgr);
		Poll::GetInstance().IOEventRegister(pChannel, true, true);
	}
	else
	{
		//阻塞就看看怎么办吧,先直接死循环
		while (1)
		{
			int accept_count = s.SocketAcceptOne(pMgr);
			if (accept_count == -1)
			{
				assert(false);
				return false;
			}
		}
	}
	return true;
}
}}

