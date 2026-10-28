#include "oi_net_session.h"
#include "oi_session_manager.h"
#include "oi_log.h"
#include "oi_net_os.h"
void NetSession::PassiveClose(int err)
{
	{
		OI::Thread::MutexGuard l(_locker);
		Close(err);
	}
	//输出日志不能持有会话锁,否则会死锁的.
	if (err && err < NET_PARSER_ERR_BEGIN)
	{
		LOG_TRACE("NetSession::PassiveClose sid=%d, system_err=%s", GetSessionID(), NetOs_StrError(err));
		return;
	}
	LOG_TRACE("NetSession::PassiveClose sid=%d, errno=%d", GetSessionID(), err);

	if (err != SESSION_MANAGER_NORMAL_CLOSE)
	{
		_p_session_mgr->CloseSession(GetSessionID());
	}
}

void NetSession::ActiveClose()
{
	OI::Thread::MutexGuard l(_locker);
	Close(SESSION_MANAGER_NORMAL_CLOSE);
}

void NetSession::Close(int err)
{
	if (_status == IO_ST_CONNECTED)
	{
		_status = IO_ST_WILL_CLOSE;
		_errcode = err;
		AllowSend();
		if (err && err < NET_PARSER_ERR_BEGIN)
		{
			//LOG_TRACE("NetSession::Close system_err=%s", strerror(err));
			return;
		}
		//LOG_TRACE("NetSession::Close errno=%d", err);
	}
}

void NetSession::OnClose()
{
	if (_p_session_mgr)
	{
		//关闭前通知Mgr会话关闭[此时必在IO线程]
		_p_session_mgr->DelSession(this);
	}
}