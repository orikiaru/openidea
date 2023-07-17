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

void Session::SwapInBuff(Bytes& inBuff)
{
	//ibuff处理在io线程,无需加锁.
	_ibuff.Append(inBuff);
	inBuff.Clear();
	OnDataIn();
}

void Session::SwapOutBuff(Bytes& outBuff)
{
	MutexGuard l(_locker);
	OnDataOut();
	outBuff.Append(_obuff);
	_obuff.Clear();
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


