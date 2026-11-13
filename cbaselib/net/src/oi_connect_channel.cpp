#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include "oi_connect_channel.h"
#include "oi_session_manager.h"
#include "oi_log.h"
#include "oi_net_os.h"
namespace OI
{
namespace Net
{

ConnectChannel::ConnectChannel(NetSocket stText, SessionManager* pMgr, uint64_t iGeneration)
    : Channel(stText, 0), m_bClose(false), m_iGeneration(iGeneration), m_pSessionManager(pMgr)
{
	// StartConnect 已切到 poll；随后 IOEventRegister 同步完成，不留下 POLL_PENDING。
	assert(Poll::GetInstance().InLoopThread());
}

void ConnectChannel::DataIn()
{
	// libuv 看到 POLLERR 会停掉 poll，回调只有 UV_EBADF、事件掩码 0，可写不会再来。
	// CheckTcpConnect 返回 false（EINPROGRESS / EINTR，Windows 的 WSAEWOULDBLOCK 也收成 EINPROGRESS）时必须在这里作废，否则等连接的人一直挂着。
	if (IsClosed())
	{
		return;
	}
	if (!CheckTcpConnect())
	{
		m_pSessionManager->AbortSession(m_iGeneration);
	}
	Close();
}

void ConnectChannel::DataOut()
{
	if (IsClosed())
	{
		return;
	}

	SESSION_TYPE iTextType = m_pSessionManager->GetSessionType();
	switch (iTextType)
	{
	case SESSION_TYPE_TCP:
		if (CheckTcpConnect())
		{
			Close();
		}
		break;
	default:
		assert(false);
		break;
	};
}

bool ConnectChannel::TryClose()
{
	if (IsClosed())
	{
		Poll::GetInstance().IOEventUnRegister(this);
		delete this;
		return true;
	}
	return false;
}

bool ConnectChannel::CheckTcpConnect()
{
	// 读 SO_ERROR。false 表示还在连：DataOut 留着等下一次可写；DataIn 不能留，poll 已经被 libuv 停掉。
	int iValue = NetOs_GetSocketError(stSocket.GetSocket());
	if (iValue == EINPROGRESS || iValue == EINTR)
	{
		return false;
	}
	if (iValue)
	{
		m_pSessionManager->AbortSession(m_iGeneration);
		return true;
	}
	// 交 dup 出来的 fd。原 fd 留给本通道关：同一个 fd 上旧 uv_poll 关闭时会 epoll DEL，新会话的监听会被一起删掉。
	net_socket_t iDescriptorLocal = NetOs_Dup(stSocket.GetSocket());
	if (iDescriptorLocal == INVALID_SOCKET_ID)
	{
		m_pSessionManager->AbortSession(m_iGeneration);
		return true;
	}
	m_pSessionManager->CompleteConnect(iDescriptorLocal, m_iGeneration);
	return true;
}

int32_t ConnectChannel::StartConnect(SessionManager* pMgr, bool bAsync)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("ConnectChannel::StartConnect");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pMgr)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	SESSION_TYPE iTextType = pMgr->GetSessionType();
	switch (iTextType)
	{
	case SESSION_TYPE_TCP:
		return StartTcpConnect(pMgr, bAsync);
	default:
		printf("ConnectChannel::StartConnect not support session_type=%d\n", iTextType);
	}
	return OI_ERR_INVALID_ARGUMENT;
}

int32_t ConnectChannel::StartTcpConnect(SessionManager* pMgr, bool bAsync)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("ConnectChannel::StartTcpConnect");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pMgr)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	NetSocket stText;
	int32_t iStatus = stText.CreateSocket(pMgr, bAsync);
	if (iStatus != OI_OK)
	{
		return iStatus;
	}

	return stText.SocketConnect(pMgr);
}
}
}
