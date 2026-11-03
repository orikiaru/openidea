#include "oi_net_session.h"
#include "oi_session_manager.h"
#include "oi_log.h"
#include "oi_net_os.h"
namespace OI
{
namespace Net
{
void NetSession::PassiveClose(int iErrorParam)
{
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		Close(iErrorParam);
	}
	//输出日志不能持有会话锁,否则会死锁的.
	if (iErrorParam > 0)
	{
		LOG_TRACE("NetSession::PassiveClose sid=%d, system_err=%s", GetSessionID(), NetOs_StrError(iErrorParam));
	}
	else
	{
		LOG_TRACE("NetSession::PassiveClose sid=%d, errno=%d", GetSessionID(), iErrorParam);
	}

	if (iErrorParam != SESSION_MANAGER_NORMAL_CLOSE)
	{
		pSessionManager->CloseSession(GetSessionID());
	}
}

void NetSession::ActiveClose()
{
	::OI::Thread::MutexGuard stLock(stLocker);
	Close(SESSION_MANAGER_NORMAL_CLOSE);
}

void NetSession::Close(int iErrorParam)
{
	if (iStatus == IO_ST_CONNECTED)
	{
		iErrorCode.store(iErrorParam, std::memory_order_relaxed);
		iStatus.store(IO_ST_WILL_CLOSE, std::memory_order_release);
		AllowSend();
		if (iErrorParam > 0)
		{
			//LOG_TRACE("NetSession::Close system_err=%s", strerror(err));
			return;
		}
		//LOG_TRACE("NetSession::Close errno=%d", err);
	}
}

void NetSession::OnClose()
{
	if (pSessionManager)
	{
		//关闭前通知Mgr会话关闭[此时必在IO线程]
		pSessionManager->DelSession(this);
	}
}
}
}
