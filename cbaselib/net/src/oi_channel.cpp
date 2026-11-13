#include "oi_channel.h"
#include "oi_errno.h"
#include "oi_session.h"
#include "oi_poll.h"
#include "oi_net_os.h"
namespace OI
{
namespace Net
{

Channel::~Channel()
{
	if (pPollToken)
	{
		Poll::GetInstance().IOEventUnRegister(this);
	}
	NetOs_Close(stSocket.GetSocket());
	stSocket.Detach();
}

int32_t Channel::AllowSend()
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("Channel::AllowSend");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	::OI::Thread::MutexGuard stLock(stLocker);
	// 关闭中的会话也要触发一次 IO，让 TryClose 回收空闲连接，不能等对端发数据。
	if (pSession && (0 == (iEventMask & IO_EV_WRITE)))
	{
		iEventMask |= IO_EV_WRITE;
		Poll::GetInstance().IOEventMod(this, iEventMask);
	}
	return OI_OK;
}

int32_t Channel::ForbidSend()
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("Channel::ForbidSend");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	::OI::Thread::MutexGuard stLock(stLocker);
	if (pSession && !pSession->IsClosed() && (iEventMask & IO_EV_WRITE))
	{
		iEventMask &= ~IO_EV_WRITE;
		Poll::GetInstance().IOEventMod(this, iEventMask);
	}
	return OI_OK;
}

int32_t Channel::AllowRead()
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("Channel::AllowRead");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	::OI::Thread::MutexGuard stLock(stLocker);
	if (pSession && !pSession->IsClosed() && (0 == (iEventMask & IO_EV_READ)))
	{
		iEventMask |= IO_EV_READ;
		Poll::GetInstance().IOEventMod(this, iEventMask);
	}
	return OI_OK;
}

int32_t Channel::ForbidRead()
{
	const int32_t iThreadStatus = Poll::GetInstance().CheckThreadAccess("Channel::ForbidRead");
	if (iThreadStatus != OI_OK)
	{
		return iThreadStatus;
	}

	::OI::Thread::MutexGuard stLock(stLocker);
	if (pSession && !pSession->IsClosed() && (iEventMask & IO_EV_READ))
	{
		iEventMask &= ~IO_EV_READ;
		Poll::GetInstance().IOEventMod(this, iEventMask);
	}
	return OI_OK;
}
}
}
