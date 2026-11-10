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

bool Session::Send(const ::OI::Base::Bytes& stBytes)
{
	return SendResult(stBytes) == OI_OK;
}

int32_t Session::SendResult(const ::OI::Base::Bytes& stBytes)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	if (IsClosed())
	{
		return OI_ERR_CONNECT;
	}
	if (iQueuedBytes > iMaxSendQueueBytes || stBytes.Size() > iMaxSendQueueBytes - iQueuedBytes)
	{
		return OI_ERR_BUSY;
	}
	stOutputBuffer.Append(stBytes);
	iQueuedBytes += stBytes.Size();
	const size_t iQueuedBeforeSend = stOutputBuffer.Size();
	const int32_t iRet = AllowSend();
	// UDP 失败时包已从缓冲取走且没有写出。缓冲变短的部分不再排队，额度退回。成功由 OnBytesSend 扣。
	if (iRet != OI_OK)
	{
		const size_t iStillQueued = stOutputBuffer.Size();
		if (iStillQueued < iQueuedBeforeSend)
		{
			const size_t iAbandoned = iQueuedBeforeSend - iStillQueued;
			assert(iAbandoned <= iQueuedBytes);
			iQueuedBytes -= iAbandoned;
		}
	}
	return iRet;
}

void Session::SetSendQueueLimit(size_t iBytes)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	iMaxSendQueueBytes = iBytes;
}

size_t Session::PendingSendBytes()
{
	::OI::Thread::MutexGuard stLock(stLocker);
	return iQueuedBytes;
}

void Session::OnBytesSend(size_t iBytes)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	assert(iBytes <= iQueuedBytes);
	iQueuedBytes -= iBytes;
}

void Session::SwapInBuff(::OI::Base::Bytes& stInputBufferLocal)
{
	if (stInputBufferLocal.Size() > ReceiveAllowance())
	{
		stInputBufferLocal.Clear();
		PassiveClose(ENOBUFS);
		return;
	}
	//ibuff处理在io线程,无需加锁.
	if (stInputBuffer.Size() == 0)
	{
		stInputBuffer.Swap(stInputBufferLocal);
	}
	else
	{
		stInputBuffer.Append(stInputBufferLocal);
	}
	stInputBufferLocal.Clear();
	OnDataIn();
}

void Session::SwapOutBuff(::OI::Base::Bytes& stOutputBufferLocal)
{
	::OI::Thread::MutexGuard stLock(stLocker);
	OnDataOut();
	if (stOutputBufferLocal.Size() == 0)
	{
		stOutputBufferLocal.Swap(stOutputBuffer);
	}
	else
	{
		stOutputBufferLocal.Append(stOutputBuffer);
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

int32_t Session::AllowSend()
{
	return pChannel ? pChannel->AllowSend() : OI_OK;
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

Session::Session(SessionManager* pMgr)
    : iStatus(IO_ST_CONNECTED), iErrorCode(0), iSessionId(0), pChannel(NULL), pSessionManager(pMgr), iReferences(1)
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
