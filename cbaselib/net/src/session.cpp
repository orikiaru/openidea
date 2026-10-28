#include <unistd.h>
#include <fcntl.h>
#include "poll.h"
#include "session.h"
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include "protocol.h"
#include "session_manager.h"
#include "channel.h"

void Session::SwapInBuff(OI::Base::Bytes& inBuff)
{
	//ibuff处理在io线程,无需加锁.
	if (_ibuff.Size() == 0)
	{
		_ibuff.Swap(inBuff);
	}
	else
	{
		_ibuff.Append(inBuff);
	}
	inBuff.Clear();
	OnDataIn();
}

void Session::SwapOutBuff(OI::Base::Bytes& outBuff)
{
	OI::Thread::MutexGuard l(_locker);
	OnDataOut();
	if (outBuff.Size() == 0)
	{
		outBuff.Swap(_obuff);
	}
	else
	{
		outBuff.Append(_obuff);
	}
	_obuff.Clear();
}

void Session::SendDrained()
{
	// 和 Send 用同一把锁：要么看到刚追加的数据，要么关写之后 Send 再把写打开。
	OI::Thread::MutexGuard l(_locker);
	OnDataOut();
	if (_obuff.Size() == 0)
	{
		ForbidSend();
	}
}

bool Session::IsClosed()
{ 
	return _status != IO_ST_CONNECTED;  
}


void Session::AllowSend()
{
	if (_p_channel)
	{
		_p_channel->AllowSend();
	}
}

void Session::ForbidSend()
{
	if (_p_channel)
	{
		_p_channel->ForbidSend();
	}
}

void Session::AllowRead()
{
	if (_p_channel)
	{
		_p_channel->AllowRead();
	}
}

void Session::ForbidRead()
{
	if (_p_channel)
	{
		_p_channel->ForbidRead();
	}
}

Session::Session(SessionManager* pMgr):_status(IO_ST_CONNECTED)
	, _errcode(0)
	, _session_id(0)
	, _p_session_mgr(pMgr)
{

}

Session::~Session()
{
}


