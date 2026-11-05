#include <sys/types.h> /* See NOTES */
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
namespace OI
{
namespace Net
{

void AcceptChannel::DataIn()
{
	for (;;)
	{
		int iAcceptCount = stSocket.SocketAcceptOne(m_pSessionManager);
		if (iAcceptCount == -1)
		{
			// 描述符用尽时连接还在队列里，监听口一直可读。先停一会儿，不让 poll 线程空转。
			Poll::GetInstance().PauseEvents(this, 100);
			break;
		}
		if (iAcceptCount == 0)
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

bool AcceptChannel::StartAccept(SessionManager* pMgr, bool bAsync)
{
	int iSessionTypeLocal = pMgr->GetSessionType();
	switch (iSessionTypeLocal)
	{
	case SESSION_TYPE_TCP:
		return StartTcpAccept(pMgr, bAsync);
	default:
		printf("SessionManager::StartServer not support session_type=%d\n", iSessionTypeLocal);
		return false;
	}
}

bool AcceptChannel::StartTcpAccept(SessionManager* pMgr, bool bAsync)
{
	// 只支持异步监听。同步 accept 会把调用线程永久堵住。
	if (!bAsync)
	{
		LOG_ERROR("AcceptChannel::StartTcpAccept sync accept not supported id=%s", pMgr->GetIDString().c_str());
		return false;
	}
	NetSocket stText;
	if (!stText.CreateSocket(pMgr, bAsync))
	{
		return false;
	}

	if (!stText.SocketListenAndBind(pMgr))
	{
		NetOs_Close(stText.GetSocket());
		stText.Detach();
		return false;
	}

	AcceptChannel* pChannelLocal = new AcceptChannel(stText, pMgr);
	Poll::GetInstance().IOEventRegister(pChannelLocal, true, true);
	return true;
}
}
}
