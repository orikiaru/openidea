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
	Poll::GetInstance().IOEventUnRegister(this);
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

	// 关闭中的会话也要触发一次 IO，让 TryClose 回收空闲连接，不能等对端发数据。
	if (pSession)
	{
		Poll::GetInstance().ChangeEvents(this, IO_EV_WRITE, 0);
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

	if (pSession && !pSession->IsClosed())
	{
		Poll::GetInstance().ChangeEvents(this, 0, IO_EV_WRITE);
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

	if (pSession && !pSession->IsClosed())
	{
		Poll::GetInstance().ChangeEvents(this, IO_EV_READ, 0);
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

	if (pSession && !pSession->IsClosed())
	{
		Poll::GetInstance().ChangeEvents(this, 0, IO_EV_READ);
	}
	return OI_OK;
}

bool Channel::SessionClosed()
{
	return pSession && pSession->IsClosed();
}
}
}
