#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include "oi_connect_channel.h"
#include "oi_session_manager.h"
#include "oi_log.h"
#include "oi_net_os.h"
namespace OI { namespace Net {

void ConnectChannel::DataIn()
{
	//对于ConnectSession来说,是IO出错了
	//不处理
	if (IsClosed())
	{
		return;
	}
}

void ConnectChannel::DataOut()
{
	if (IsClosed())
	{
		return;
	}

	SESSION_TYPE s_type = _p_session_mgr->GetSessionType();
	switch (s_type)
	{
		case SESSION_TYPE_TCP:
			CheckTcpConnect();
			Close();
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
	//The  socket  is  nonblocking  and  the connection cannot be completed immediately.  
	//It is possible to select(2) or poll(2) for completion by selecting the socket for writing.  After select(2)indicates writability, 
	//use getsockopt(2) to read the SO_ERROR option at level SOL_SOCKET to determine whether connect() completed successfully (SO_ERROR is zero) or  
	//unsuccessfully  (SO_ERROR is one of the usual error codes listed here, explaining the reason for the failure).
	//       int getsockopt(int sockfd, int level, int optname, void *optval, socklen_t *optlen);
	int value = NetOs_GetSocketError(_socket.GetSocket());
	bool finish = false;
	if (value)
	{
		// 待改：EINPROGRESS / EINTR 应留下等下一次可写，不要 AbortSession。
		// DataOut 在这次调用后无条件 Close，通道会被拆掉，下次也等不到。
		// 见 docs/sequence-diagram 里 single-thread 和 multi-thread 的 connect.md。
		if (value == EINPROGRESS || value == EINTR)
		{

		}
		_p_session_mgr->AbortSession();
	}
	else
	{
		finish = true;
		// 待改：不要 Dup。复制失败会把无效句柄交进 AddSession，原 fd 还要在通道析构里关掉。
		// 应把当前这只套接字交给会话。见 docs/sequence-diagram 里两边的 connect.md。
		_p_session_mgr->AddSession(NetOs_Dup(_socket.GetSocket()));
	}
	//普通tcp连接无论建立成功或失败都关闭连接会话
	return finish;
}

bool ConnectChannel::StartConnect(SessionManager* pMgr, bool async)
{
	SESSION_TYPE s_type = pMgr->GetSessionType();
	switch (s_type)
	{
	case SESSION_TYPE_TCP:
		return StartTcpConnect(pMgr, async);
	default:
		printf("ConnectChannel::StartConnect not support session_type=%d\n", s_type);
		assert(false);
	}
	return false;

}

bool ConnectChannel::StartTcpConnect(SessionManager* pMgr, bool async)
{
	NetSocket s;
	if (!s.CreateSocket(pMgr, async))
	{
		return false;
	}

	return s.SocketConnect(pMgr);
}
}}