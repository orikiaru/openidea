#include <unistd.h>
#include <fcntl.h>
#include "oi_poll.h"
#include "oi_session.h"
#include <sys/types.h>          /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include "oi_session_manager.h"
#include "oi_channel.h"
namespace OI { namespace Net {

bool Session::Send(const ::OI::Base::Bytes& bytes)
{
	::OI::Thread::MutexGuard lock(_locker);
	if (IsClosed() || _queued_bytes > _max_send_queue_bytes
		|| bytes.Size() > _max_send_queue_bytes - _queued_bytes) return false;
	_obuff.Append(bytes);
	_queued_bytes += bytes.Size();
	AllowSend();
	return true;
}

void Session::SetSendQueueLimit(size_t bytes)
{
	::OI::Thread::MutexGuard lock(_locker);
	_max_send_queue_bytes = bytes;
}

size_t Session::PendingSendBytes()
{
	::OI::Thread::MutexGuard lock(_locker);
	return _queued_bytes;
}

void Session::OnBytesSent(size_t bytes)
{
	::OI::Thread::MutexGuard lock(_locker);
	assert(bytes <= _queued_bytes);
	_queued_bytes -= bytes;
}

void Session::SwapInBuff(::OI::Base::Bytes& inBuff)
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

void Session::SwapOutBuff(::OI::Base::Bytes& outBuff)
{
	::OI::Thread::MutexGuard l(_locker);
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
	::OI::Thread::MutexGuard l(_locker);
	OnDataOut();
	if (_obuff.Size() == 0)
	{
		ForbidSend();
	}
}

bool Session::IsClosed()
{
	return _status.load(std::memory_order_acquire) != IO_ST_CONNECTED;
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
	, _p_channel(NULL)
	, _p_session_mgr(pMgr)
	, _refs(1)
{

}

Session::~Session()
{
}

void Session::Release()
{
	if (_refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete this;
	}
}

void Session::DetachChannel()
{
	::OI::Thread::MutexGuard l(_locker);
	_p_channel = NULL;
}
}}


