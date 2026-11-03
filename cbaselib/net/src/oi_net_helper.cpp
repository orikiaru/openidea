#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>

#include "oi_net_helper.h"
#include "oi_session_manager.h"
#include "oi_log.h"
#include "oi_socket.h"
#include "oi_connect_channel.h"
#include "oi_net_os.h"
namespace OI { namespace Net {


bool NetHelper::CreateSocket(NetSocket& stNetSocketParam, SessionManager* pMgrParam, bool bAsyncParam)
{
	if (!bAsyncParam)
	{
		LOG_ERROR("NetHelper::CreateSocket sync socket not supported id=%s", pMgrParam->GetIDString().c_str());
		return false;
	}
	net_socket_t iSfd = NetOs_CreateTcp(bAsyncParam);
	if (iSfd == INVALID_SOCKET_ID)
	{
		LOG_ERROR("NetHelper::CreateSocket id=%s, err=%s", pMgrParam->GetIDString().c_str(), NetOs_StrError(errno));
		return false;
	}
	if (!stNetSocketParam.Init(iSfd, pMgrParam))
	{
		LOG_ERROR("NetHelper::CreateSocket Init error id=%s", pMgrParam->GetIDString().c_str());
		NetOs_Close(iSfd);
		stNetSocketParam.Detach();
		return false;
	}
	stNetSocketParam.SetAsync(bAsyncParam);
	return true;
}

bool NetHelper::SocketBind(NetSocket& stSessionIdParam, SessionManager* pMgrParam)
{
	if (!stSessionIdParam.IsValidAttr())
	{
		return false;
	}
	if (stSessionIdParam.IsBind())
	{
		return false;
	}

	int iReturnCodeLocal = NetOs_Bind(stSessionIdParam.GetSocket(), stSessionIdParam.GetAddr().NetAttrPtr(), stSessionIdParam.GetAddr().NetAttrLen());
	if (iReturnCodeLocal != 0)
	{
		LOG_TRACE("NetHelper::SocketBind error=%s, SessionManager=%s", NetOs_StrError(errno), pMgrParam->GetIDString().c_str());
		return false;
	}
	stSessionIdParam.SetBind();
	return true;
}

bool NetHelper::SocketListen(NetSocket& stSessionIdParam, SessionManager* pMgrParam)
{
	if (!stSessionIdParam.IsBind())
	{
		return false;
	}
	int iReturnCodeLocal = NetOs_Listen(stSessionIdParam.GetSocket(), SOMAXCONN);
	if (iReturnCodeLocal != 0)
	{
		LOG_TRACE("NetHelper::SocketListen error=%s, SessionManager=%s", NetOs_StrError(errno), pMgrParam->GetIDString().c_str());
		return false;
	}
	stSessionIdParam.SetListen();
	return true;
}

bool NetHelper::SocketListenAndBind(NetSocket& stSessionIdParam, SessionManager* pMgrParam)
{
	if (!SocketBind(stSessionIdParam, pMgrParam))
	{
		return false;
	}
	if (!SocketListen(stSessionIdParam, pMgrParam))
	{
		return false;
	}
	return true;
}

bool NetHelper::SocketConnect(NetSocket& stSessionIdParam, SessionManager* pMgrParam)
{
	if (!stSessionIdParam.IsValidAttr())
	{
		return false;
	}
	if (stSessionIdParam.IsAccept())
	{
		return false;
	}

	bool bSucc = false;

	int iReturnCodeLocal = NetOs_Connect(stSessionIdParam.GetSocket(), stSessionIdParam.GetAddr().NetAttrPtr(), stSessionIdParam.GetAddr().NetAttrLen());
	if (iReturnCodeLocal == -1 && errno == EINPROGRESS)
	{
		ConnectChannel* pChannelLocal = new ConnectChannel(stSessionIdParam, pMgrParam);
		Poll::GetInstance().IOEventRegister(pChannelLocal, false, true);
		bSucc = true;
	}
	else if (iReturnCodeLocal == 0)
	{
		pMgrParam->AddSession(stSessionIdParam.GetSocket());
		bSucc = true;
	}

	if (!bSucc)
	{
		LOG_ERROR("NetHelper::SocketConnect id=%s, err=%s", pMgrParam->GetIDString().c_str(), NetOs_StrError(errno));
		NetOs_Close(stSessionIdParam.GetSocket());
		stSessionIdParam.Detach();
		pMgrParam->AbortSession();
		return false;
	}

	return true;
}

int NetHelper::SocketAcceptOne(NetSocket& stSessionIdParam, SessionManager* pMgrParam)
{
	if (!stSessionIdParam.IsValidAttr())
	{
		return -1;
	}
	if (!stSessionIdParam.IsBind() || !stSessionIdParam.IsListen())
	{
		return -1;
	}
	if (stSessionIdParam.IsConnect())
	{
		return -1;
	}

	stSessionIdParam.SetAccept();
	int iAccpetCount = -1;
	for (;;)
	{
		net_socket_t iCfd = NetOs_Accept(stSessionIdParam.GetSocket());
		if (iCfd != INVALID_SOCKET_ID)
		{
			pMgrParam->AddSession(iCfd);
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
		LOG_ERROR("NetHelper::SocketAccept error id=%s, error=%s", pMgrParam->GetIDString().c_str(), NetOs_StrError(errno));
		iAccpetCount = -1;
		break;
	}

	return iAccpetCount;
}


NetAddr NetHelper::GetNetAddr(SessionManager* pMgrParam)
{
	NetAddr stAddr;
	stAddr.Init(pMgrParam);
	return stAddr;
}
}}
