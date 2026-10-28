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
		//没链接完毕或者被中断等下次触发?
		//暂时先等重连吧
		if (value == EINPROGRESS || value == EINTR)
		{

		}
		_p_session_mgr->AbortSession();
	}
	else
	{
		finish = true;
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