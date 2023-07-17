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


bool NetHelper::CreateSocket(NetSocket& net_socket, SessionManager* pMgr, bool async)
{
	assert(async == true && u8"目前不支持同步Server,可以考虑起单独线程/协程维护?");
	//默认不允许子进程继承套接字描述符
	int sock_mask = SOCK_CLOEXEC;
	sock_mask |= SOCK_STREAM;
	sock_mask |= (async ? SOCK_NONBLOCK : 0);

	int proto_type = AF_INET;
	//TODO AF_INET6
	net_socket_t sfd = socket(proto_type, sock_mask, 0);
	if (sfd == -1)
	{
		LOG_ERROR("NetHelper::CreateSocket id=%s, err=%s", pMgr->GetIDString().c_str(), strerror(errno));
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

	int ret = bind(sid.GetSocket(), sid.GetAddr().NetAttrPtr(), sid.GetAddr().NetAttrLen());
	if (ret != 0)
	{
		LOG_TRACE("NetHelper::SocketBind error=%s, SessionManager=%s", strerror(errno), pMgr->GetIDString().c_str());
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
	//TODO Config
	int ret = listen(sid.GetSocket(), listen_count);
	if (ret != 0)
	{
		LOG_TRACE("NetHelper::SocketListen error=%s, SessionManager=%s", strerror(errno), pMgr->GetIDString().c_str());
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

	int ret = connect(sid.GetSocket(), sid.GetAddr().NetAttrPtr(), sid.GetAddr().NetAttrLen());
	if (ret == -1 && errno == EINPROGRESS)
	{
		ConnectChannel* pChannel = new ConnectChannel(sid, pMgr);
		Poll::GetInstance()->IOEventRegister(pChannel, false, true);
		succ = true;
	}
	else if (ret == 0)
	{
		pMgr->AddSession(sid.GetSocket());
		succ = true;
	}

	if (!succ)
	{
		LOG_ERROR("NetHelper::SocketConnect id=%s, err=%s", pMgr->GetIDString().c_str(), strerror(errno));
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
		int cfd = accept(sid.GetSocket(), 0, 0);
		if (cfd > 0)
		{
			pMgr->AddSession(cfd);
			accpet_count = 1;
			break;
		}
		else if (cfd == -1 && errno == EAGAIN)
		{
			accpet_count = 0;
			break;
		}
		else if (cfd == -1 && errno == EINTR)
		{
			continue;
		}
		LOG_ERROR("NetHelper::SocketAccept error id=%s, error=%s", pMgr->GetIDString().c_str(), strerror(errno));
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
