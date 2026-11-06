#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h> /* See NOTES */
#include <sys/socket.h>

#include "oi_net_helper.h"
#include "oi_session_manager.h"
#include "oi_log.h"
#include "oi_socket.h"
#include "oi_connect_channel.h"
#include "oi_net_os.h"
namespace OI
{
namespace Net
{

bool NetHelper::CreateSocket(NetSocket& stNetSocket, SessionManager* pMgr, bool bAsync)
{
	if (!bAsync)
	{
		LOG_ERROR("NetHelper::CreateSocket sync socket not supported id=%s", pMgr->GetIDString().c_str());
		return false;
	}
	if (!stNetSocket.GetAddr().Init(pMgr))
	{
		LOG_ERROR("NetHelper::CreateSocket Init error id=%s", pMgr->GetIDString().c_str());
		return false;
	}
	net_socket_t iSfd = stNetSocket.GetAddr().CreateTcp(bAsync);
	if (iSfd == INVALID_SOCKET_ID)
	{
		LOG_ERROR("NetHelper::CreateSocket id=%s, err=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		return false;
	}
	stNetSocket.Attach(iSfd);
	stNetSocket.SetAsync(bAsync);
	return true;
}

bool NetHelper::SocketBind(NetSocket& stSessionId, SessionManager* pMgr)
{
	if (!stSessionId.IsValidAttr())
	{
		return false;
	}
	if (stSessionId.IsBind())
	{
		return false;
	}

	int iReturnCodeLocal = NetOs_Bind(stSessionId.GetSocket(), stSessionId.GetAddr().NetAttrPtr(), stSessionId.GetAddr().NetAttrLen());
	if (iReturnCodeLocal != 0)
	{
		LOG_TRACE("NetHelper::SocketBind error=%s, SessionManager=%s", NetOs_StrError(errno), pMgr->GetIDString().c_str());
		return false;
	}
	stSessionId.SetBind();
	return true;
}

bool NetHelper::SocketListen(NetSocket& stSessionId, SessionManager* pMgr)
{
	if (!stSessionId.IsBind())
	{
		return false;
	}
	int iReturnCodeLocal = NetOs_Listen(stSessionId.GetSocket(), SOMAXCONN);
	if (iReturnCodeLocal != 0)
	{
		LOG_TRACE("NetHelper::SocketListen error=%s, SessionManager=%s", NetOs_StrError(errno), pMgr->GetIDString().c_str());
		return false;
	}
	stSessionId.SetListen();
	return true;
}

bool NetHelper::SocketListenAndBind(NetSocket& stSessionId, SessionManager* pMgr)
{
	if (!SocketBind(stSessionId, pMgr))
	{
		return false;
	}
	if (!SocketListen(stSessionId, pMgr))
	{
		return false;
	}
	return true;
}

bool NetHelper::SocketConnect(NetSocket& stSessionId, SessionManager* pMgr)
{
	if (!stSessionId.IsValidAttr())
	{
		return false;
	}
	if (stSessionId.IsAccept())
	{
		return false;
	}

	bool bSucc = false;

	int iReturnCodeLocal = NetOs_Connect(stSessionId.GetSocket(), stSessionId.GetAddr().NetAttrPtr(), stSessionId.GetAddr().NetAttrLen());
	if (iReturnCodeLocal == -1 && errno == EINPROGRESS)
	{
		ConnectChannel* pChannelLocal = new ConnectChannel(stSessionId, pMgr, pMgr->iConnectGeneration);
		Poll::GetInstance().IOEventRegister(pChannelLocal, false, true);
		bSucc = true;
	}
	else if (iReturnCodeLocal == 0)
	{
		pMgr->CompleteConnect(stSessionId.GetSocket(), pMgr->iConnectGeneration);
		bSucc = true;
	}

	if (!bSucc)
	{
		LOG_ERROR("NetHelper::SocketConnect id=%s, err=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		NetOs_Close(stSessionId.GetSocket());
		stSessionId.Detach();
		pMgr->AbortSession(pMgr->iConnectGeneration);
		return false;
	}

	return true;
}

int NetHelper::SocketAcceptOne(NetSocket& stSessionId, SessionManager* pMgr)
{
	if (!stSessionId.IsValidAttr())
	{
		return -1;
	}
	if (!stSessionId.IsBind() || !stSessionId.IsListen())
	{
		return -1;
	}
	if (stSessionId.IsConnect())
	{
		return -1;
	}

	stSessionId.SetAccept();
	int iAccpetCount = -1;
	for (;;)
	{
		net_socket_t iCfd = NetOs_Accept(stSessionId.GetSocket());
		if (iCfd != INVALID_SOCKET_ID)
		{
			pMgr->AddSession(iCfd);
			iAccpetCount = 1;
			break;
		}
		else if (iCfd == INVALID_SOCKET_ID && (errno == EAGAIN || errno == EINPROGRESS))
		{
			iAccpetCount = 0;
			break;
		}
		else if (iCfd == INVALID_SOCKET_ID && (errno == EINTR || errno == ECONNABORTED || errno == EPROTO))
		{
			// 对端在 accept 之前就断了，只影响这一条，接着收下一条。
			continue;
		}
		LOG_ERROR("NetHelper::SocketAccept error id=%s, error=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		iAccpetCount = -1;
		break;
	}

	return iAccpetCount;
}

NetAddr NetHelper::GetNetAddr(SessionManager* pMgr)
{
	NetAddr stAddr;
	stAddr.Init(pMgr);
	return stAddr;
}
}
}
