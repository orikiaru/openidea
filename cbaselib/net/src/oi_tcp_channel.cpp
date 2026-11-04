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
	for (;;)
	{
		const size_t iAllowance = pSession->ReceiveAllowance();
		if (stInputBuffer.Size() >= iAllowance)
		{
			// 满额时先交给解析器。只有解析后仍无空间才拒绝，不能误杀完整合法包。
			pSession->SwapInBuff(stInputBuffer);
			if (!pSession->IsClosed() && pSession->ReceiveAllowance() == 0)
			{
				pSession->PassiveClose(ENOBUFS);
			}
			return;
		}
		if (iRound >= iItemIndexRoundMax)
		{
			goto read_finish;
		}
		if (stInputBuffer.Capacity() - stInputBuffer.Size() < 16 * 1024)
		{
			stInputBuffer.Reserve(stInputBuffer.Size() + 64 * 1024);
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
			goto read_finish;
		}
		else if (iReadNumber == -1 && errno == EINTR)
		{
			continue;
		}
		if (iReadNumber == 0)
		{
			goto peer_close;
		}
		goto read_fault;
	}
read_finish:
	pSession->SwapInBuff(stInputBuffer);
	return;
read_fault:
	pSession->SwapInBuff(stInputBuffer);
	pSession->PassiveClose(errno);
	return;
peer_close:
	pSession->SwapInBuff(stInputBuffer);
	pSession->PassiveClose(0);
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
			pSession->OnBytesSent((size_t)iWriteNumber);
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
