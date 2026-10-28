#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include <unistd.h>
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>

#include "net_helper.h"
#include "session_manager.h"
#include "logger.h"
#include "net_socket.h"
#include "connect_channel.h"
#include "net_os.h"


bool NetHelper::CreateSocket(NetSocket& net_socket, SessionManager* pMgr, bool async)
{
	assert(async == true && "sync server is not supported");
	net_socket_t sfd = NetOs_CreateTcp(async);
	if (sfd == INVALID_SOCKET_ID)
	{
		LOG_ERROR("NetHelper::CreateSocket id=%s, err=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		return false;
	}
	if (!net_socket.Init(sfd, pMgr))
	{
		LOG_ERROR("NetHelper::CreateSocket Init error id=%s", pMgr->GetIDString().c_str());
		return false;
	}
	net_socket.SetAsync(async);
	return true;
}

bool NetHelper::SocketBind(NetSocket& sid, SessionManager* pMgr)
{
	if (!sid.IsValidAttr())
	{
		return false;
	}
	if (sid.IsBind())
	{
		return false;
	}

	int ret = NetOs_Bind(sid.GetSocket(), sid.GetAddr().NetAttrPtr(), sid.GetAddr().NetAttrLen());
	if (ret != 0)
	{
		LOG_TRACE("NetHelper::SocketBind error=%s, SessionManager=%s", NetOs_StrError(errno), pMgr->GetIDString().c_str());
		return false;
	}
	sid.SetBind();
	return true;
}

bool NetHelper::SocketListen(NetSocket& sid, SessionManager* pMgr)
{
	if (!sid.IsBind())
	{
		return false;
	}
	int listen_count = 10;
	//TODO OI::Base::Config
	int ret = NetOs_Listen(sid.GetSocket(), listen_count);
	if (ret != 0)
	{
		LOG_TRACE("NetHelper::SocketListen error=%s, SessionManager=%s", NetOs_StrError(errno), pMgr->GetIDString().c_str());
		return false;
	}
	sid.SetListen();
	return true;
}

bool NetHelper::SocketListenAndBind(NetSocket& sid, SessionManager* pMgr)
{
	if (!SocketBind(sid, pMgr))
	{
		return false;
	}
	if (!SocketListen(sid, pMgr))
	{
		return false;
	}
	return true;
}

bool NetHelper::SocketConnect(NetSocket& sid, SessionManager* pMgr)
{
	if (!sid.IsValidAttr())
	{
		return false;
	}
	if (sid.IsAccept())
	{
		return false;
	}

	bool succ = false;

	int ret = NetOs_Connect(sid.GetSocket(), sid.GetAddr().NetAttrPtr(), sid.GetAddr().NetAttrLen());
	if (ret == -1 && errno == EINPROGRESS)
	{
		ConnectChannel* pChannel = new ConnectChannel(sid, pMgr);
		Poll::GetInstance().IOEventRegister(pChannel, false, true);
		succ = true;
	}
	else if (ret == 0)
	{
		pMgr->AddSession(sid.GetSocket());
		succ = true;
	}

	if (!succ)
	{
		LOG_ERROR("NetHelper::SocketConnect id=%s, err=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		return false;
	}

	return true;
}

int NetHelper::SocketAcceptOne(NetSocket& sid, SessionManager* pMgr)
{
	if (!sid.IsValidAttr())
	{
		return -1;
	}
	if (!sid.IsBind() || !sid.IsListen())
	{
		return -1;
	}
	if (sid.IsConnect())
	{
		return -1;
	}

	sid.SetAccept();
	int accpet_count = -1;
	for (;;)
	{
		net_socket_t cfd = NetOs_Accept(sid.GetSocket());
		if (cfd != INVALID_SOCKET_ID)
		{
			pMgr->AddSession(cfd);
			accpet_count = 1;
			break;
		}
		else if (cfd == INVALID_SOCKET_ID && (errno == EAGAIN || errno == EINPROGRESS))
		{
			accpet_count = 0;
			break;
		}
		else if (cfd == INVALID_SOCKET_ID && errno == EINTR)
		{
			continue;
		}
		LOG_ERROR("NetHelper::SocketAccept error id=%s, error=%s", pMgr->GetIDString().c_str(), NetOs_StrError(errno));
		accpet_count = -1;
		break;
	}

	return accpet_count;
}


NetAddr NetHelper::GetNetAddr(SessionManager* pMgr)
{
	NetAddr addr;
	addr.Init(pMgr);
	return addr;
}
