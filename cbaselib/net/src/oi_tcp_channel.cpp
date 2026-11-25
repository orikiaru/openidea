#include "oi_tcp_channel.h"
#include "oi_session.h"
#include "oi_net_os.h"
#include <algorithm>
namespace OI
{
namespace Net
{

void TCP_Channel::DataIn()
{
	// 水平触发。一轮读够上限就先交给会话，剩下的下一轮再进来。
	const size_t iItemIndexRoundMax = 4 * 1024 * 1024;
	size_t iRound = 0;
	int iReadError = 0;
	bool bReadEndedLocal = false;
	for (;;)
	{
		const size_t iAllowance = pSession->ReceiveAllowance();
		if (stInputBuffer.Size() >= iAllowance)
		{
			// 满额时先交给解析器。只有解析后仍无空间才拒绝，不能误杀完整合法包。
			pSession->SwapInBuff(stInputBuffer);
			if (!pSession->IsClosed() && (iEventMask & IO_EV_READ) && pSession->ReceiveAllowance() == 0)
			{
				pSession->PassiveClose(ENOBUFS);
			}
			return;
		}
		if (iRound >= iItemIndexRoundMax)
		{
			break;
		}
		if (stInputBuffer.Size() == stInputBuffer.Capacity())
		{
			const size_t iGrowBytes = std::min(iAllowance - stInputBuffer.Size(), std::max<size_t>(4096, stInputBuffer.Size()));
			stInputBuffer.Reserve(stInputBuffer.Size() + iGrowBytes, iAllowance);
		}
		const size_t iReadLimit = std::min({stInputBuffer.Capacity() - stInputBuffer.Size(),
		    iAllowance - stInputBuffer.Size(), iItemIndexRoundMax - iRound});
		int iReadNumber = NetOs_Recv(stSocket.GetSocket(), stInputBuffer.End(), iReadLimit);
		if (iReadNumber > 0)
		{
			stInputBuffer.Commit((size_t)iReadNumber);
			iRound += (size_t)iReadNumber;
			continue;
		}
		else if (iReadNumber == -1 && errno == EAGAIN)
		{
			break;
		}
		else if (iReadNumber == -1 && errno == EINTR)
		{
			continue;
		}
		iReadError = iReadNumber == 0 ? 0 : errno;
		bReadEndedLocal = true;
		break;
	}
	if (bReadEndedLocal)
	{
		pSession->bReadEnded = true;
	}
	pSession->SwapInBuff(stInputBuffer);
	// 接收方可能正在等应用消费最后一片。恢复读事件后，再交付 EOF。
	if (bReadEndedLocal && (iReadError || (iEventMask & IO_EV_READ)))
	{
		pSession->PassiveClose(iReadError);
	}
	return;
}

void TCP_Channel::DataOut()
{
	pSession->SwapOutBuff(stOutputBuffer);
	while (stOutputBuffer.Size())
	{
		int iWriteNumber = NetOs_Send(stSocket.GetSocket(), stOutputBuffer.Begin(), stOutputBuffer.Size());
		if (iWriteNumber > 0)
		{
			stOutputBuffer.Erase(0, iWriteNumber);
			pSession->OnBytesSend((size_t)iWriteNumber);
			continue;
		}
		else if (iWriteNumber == -1 && errno == EAGAIN)
		{
			goto write_finish;
		}
		else if (iWriteNumber == -1 && errno == EINTR)
		{
			continue;
		}
		goto write_fault;
	}
write_finish:
	if (stOutputBuffer.Size() == 0)
	{
		pSession->SendDrained();
	}
	return;
write_fault:
	pSession->PassiveClose(errno);
	return;
}
}
}
