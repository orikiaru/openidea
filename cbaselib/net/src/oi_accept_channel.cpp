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

	if (!bAsync)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	return Poll::GetInstance().StartListener([](void* pArgument)
	{
		auto* pManager = static_cast<SessionManager*>(pArgument);
		switch (pManager->GetSessionType())
		{
		case SESSION_TYPE_TCP:
			return StartTcpAccept(pManager, true);
		case SESSION_TYPE_UDP:
			return UDP_Channel::StartBind(pManager);
		default:
			return static_cast<int32_t>(OI_ERR_INVALID_ARGUMENT);
		}
	}, pMgr);
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
	const int32_t iRegisterStatus = Poll::GetInstance().IOEventRegister(pChannelLocal, true, true);
	if (iRegisterStatus != OI_OK)
	{
		delete pChannelLocal;
		return iRegisterStatus;
	}
	pMgr->pAcceptChannel = pChannelLocal;
	return OI_OK;
}

void AcceptChannel::StopAccept(SessionManager* pMgr)
{
	if (!pMgr)
	{
		return;
	}
	AcceptChannel* pChannelLocal = pMgr->pAcceptChannel;
	pMgr->pAcceptChannel = nullptr;
	if (!pChannelLocal)
	{
		return;
	}
	Poll::GetInstance().IOEventUnRegister(pChannelLocal);
	delete pChannelLocal;
}
}
}
