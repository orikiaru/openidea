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

int32_t NetHelper::CreateSocket(NetSocket& stNetSocket, SessionManager* pMgr, bool bAsync)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("NetHelper::CreateSocket");
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
		LOG_ERROR("NetHelper::CreateSocket sync socket not supported id=%s", pMgr->GetIDString().c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (!stNetSocket.GetAddr().Init(pMgr))
	{
		LOG_ERROR("NetHelper::CreateSocket Init error id=%s", pMgr->GetIDString().c_str());
		return OI_ERR_INVALID_ARGUMENT;
	}
	net_socket_t iSfd = stNetSocket.GetAddr().CreateTcp(bAsync);
	if (iSfd == INVALID_SOCKET_ID)
	{
		LOG_ERROR("NetHelper::CreateSocket id=%s, err=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		return OI_ERR_SOCKET;
	}
	stNetSocket.Attach(iSfd);
	stNetSocket.SetAsync(bAsync);
	return OI_OK;
}

int32_t NetHelper::SocketBind(NetSocket& stSessionId, SessionManager* pMgr)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("NetHelper::SocketBind");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pMgr)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	if (!stSessionId.IsValidAttr())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (stSessionId.IsBind())
	{
		return OI_ERR_EXIST;
	}

	int iReturnCodeLocal = NetOs_Bind(stSessionId.GetSocket(), stSessionId.GetAddr().NetAttrPtr(), stSessionId.GetAddr().NetAttrLen());
	if (iReturnCodeLocal != 0)
	{
		LOG_TRACE("NetHelper::SocketBind error=%s, SessionManager=%s", NetOs_StrError(errno), pMgr->GetIDString().c_str());
		return OI_ERR_BIND;
	}
	stSessionId.SetBind();
	return OI_OK;
}

int32_t NetHelper::SocketListen(NetSocket& stSessionId, SessionManager* pMgr)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("NetHelper::SocketListen");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pMgr)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	if (!stSessionId.IsBind())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	int iReturnCodeLocal = NetOs_Listen(stSessionId.GetSocket(), SOMAXCONN);
	if (iReturnCodeLocal != 0)
	{
		LOG_TRACE("NetHelper::SocketListen error=%s, SessionManager=%s", NetOs_StrError(errno), pMgr->GetIDString().c_str());
		return OI_ERR_LISTEN;
	}
	stSessionId.SetListen();
	return OI_OK;
}

int32_t NetHelper::SocketListenAndBind(NetSocket& stSessionId, SessionManager* pMgr)
{
	const int32_t iStatus = SocketBind(stSessionId, pMgr);
	if (iStatus != OI_OK)
	{
		return iStatus;
	}
	return SocketListen(stSessionId, pMgr);
}

int32_t NetHelper::SocketConnect(NetSocket& stSessionId, SessionManager* pMgr)
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("NetHelper::SocketConnect");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}
	if (!pMgr)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}

	if (!stSessionId.IsValidAttr())
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (stSessionId.IsAccept())
	{
		return OI_ERR_INVALID_ARGUMENT;
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
		return OI_ERR_CONNECT;
	}

	return OI_OK;
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
