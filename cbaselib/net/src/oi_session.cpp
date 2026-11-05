#include <unistd.h>
#include <fcntl.h>
#include "oi_poll.h"
#include "oi_session.h"
#include <sys/types.h> /* See NOTES */
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/ip.h> /* superset of previous */
#include <arpa/inet.h>
#include "oi_session_manager.h"
#include "oi_channel.h"
namespace OI
{
namespace Net
{

bool Session::Send(const ::OI::Base::Bytes& stBytesParam)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	if (IsClosed() || iQueuedBytes > iMaxSendQueueBytes || stBytesParam.Size() > iMaxSendQueueBytes - iQueuedBytes)
	{
		return false;
	}
	stOutputBuffer.Append(stBytesParam);
	iQueuedBytes += stBytesParam.Size();
	AllowSend();
	return true;
}

void Session::SetSendQueueLimit(size_t iBytesParam)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	iMaxSendQueueBytes = iBytesParam;
}

size_t Session::PendingSendBytes()
{
	::OI::Thread::MutexGuard stLock(stLocker);
	return iQueuedBytes;
}

void Session::OnBytesSent(size_t iBytesParam)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	assert(iBytesParam <= iQueuedBytes);
	iQueuedBytes -= iBytesParam;
}

void Session::SwapInBuff(::OI::Base::Bytes& stInputBufferParam)
{
	if (stInputBufferParam.Size() > ReceiveAllowance())
	{
		stInputBufferParam.Clear();
		PassiveClose(ENOBUFS);
		return;
	}
	//ibuff处理在io线程,无需加锁.
	if (stInputBuffer.Size() == 0)
	{
		stInputBuffer.Swap(stInputBufferParam);
	}
	else
	{
		stInputBuffer.Append(stInputBufferParam);
	}
	stInputBufferParam.Clear();
	OnDataIn();
}

void Session::SwapOutBuff(::OI::Base::Bytes& stOutputBufferParam)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	OnDataOut();
	if (stOutputBufferParam.Size() == 0)
	{
		stOutputBufferParam.Swap(stOutputBuffer);
	}
	else
	{
		stOutputBufferParam.Append(stOutputBuffer);
	}
	stOutputBuffer.Clear();
}

void Session::SendDrained()
{
	// 和 Send 用同一把锁：要么看到刚追加的数据，要么关写之后 Send 再把写打开。
	::OI::Thread::MutexGuard stLock(stLocker);
	OnDataOut();
	if (stOutputBuffer.Size() == 0)
	{
		ForbidSend();
	}
}

bool Session::IsClosed()
{
	return iStatus.load(std::memory_order_acquire) != IO_ST_CONNECTED;
}

void Session::AllowSend()
{
	if (pChannel)
	{
		pChannel->AllowSend();
	}
}

void Session::ForbidSend()
{
	if (pChannel)
	{
		pChannel->ForbidSend();
	}
}

void Session::AllowRead()
{
	if (pChannel)
	{
		pChannel->AllowRead();
	}
}

void Session::ForbidRead()
{
	if (pChannel)
	{
		pChannel->ForbidRead();
	}
}

Session::Session(SessionManager* pMgrParam)
    : iStatus(IO_ST_CONNECTED), iErrorCode(0), iSessionId(0), pChannel(NULL), pSessionManager(pMgrParam), iReferences(1)
{
}

Session::~Session()
{
}

void Session::Release()
{
	if (iReferences.fetch_sub(1, std::memory_order_acq_rel) == 1)
	{
		delete this;
	}
}

void Session::DetachChannel()
{
	::OI::Thread::MutexGuard stLock(stLocker);
	pChannel = NULL;
}
}
}
