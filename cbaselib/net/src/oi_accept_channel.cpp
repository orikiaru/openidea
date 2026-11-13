#include <sys/types.h> /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>

#include "oi_accept_channel.h"
#include "oi_session_manager.h"
#include "oi_udp_channel.h"
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

int32_t AcceptChannel::StartAccept(SessionManager* pMgr, bool bAsync)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("AcceptChannel::StartAccept");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pMgr)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	int iSessionTypeLocal = pMgr->GetSessionType();
	switch (iSessionTypeLocal)
	{
	case SESSION_TYPE_TCP:
		return StartTcpAccept(pMgr, bAsync);
	case SESSION_TYPE_UDP:
		// UDP 没有 accept：绑上口，收到没见过的来源就新建会话。
		return UDP_Channel::StartBind(pMgr);
	default:
		printf("SessionManager::StartServer not support session_type=%d\n", iSessionTypeLocal);
		return OI_ERR_INVALID_ARGUMENT;
	}
}

int32_t AcceptChannel::StartTcpAccept(SessionManager* pMgr, bool bAsync)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("AcceptChannel::StartTcpAccept");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pMgr)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	// 只支持异步监听。同步 accept 会把调用线程永久堵住。
	if (!bAsync)
	{
		LOG_ERROR("AcceptChannel::StartTcpAccept sync accept not supported id=%s", pMgr->GetIDString().c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	NetSocket stText;
	int32_t iStatus = stText.CreateSocket(pMgr, bAsync);
	if (iStatus != OI_OK)
	{
		return iStatus;
	}

	iStatus = stText.SocketListenAndBind(pMgr);
	if (iStatus != OI_OK)
	{
		NetOs_Close(stText.GetSocket());
		stText.Detach();
		return iStatus;
	}

	AcceptChannel* pChannelLocal = new AcceptChannel(stText, pMgr);
	Poll::GetInstance().IOEventRegister(pChannelLocal, true, true);
	return OI_OK;
}
}
}
