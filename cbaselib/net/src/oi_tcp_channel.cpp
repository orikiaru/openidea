#include "oi_tcp_channel.h"
#include "oi_session.h"
#include "oi_net_os.h"
namespace OI { namespace Net {

void TCP_Channel::DataIn()
{
	// 水平触发。一轮读够上限就先交给会话，剩下的下一轮再进来。
	const size_t kRoundMax = 4 * 1024 * 1024;
	size_t round = 0;
	for (;;)
	{
		if (round >= kRoundMax)
		{
			goto read_finish;
		}
		if (_ibuff.Captity() - _ibuff.Size() < 16 * 1024)
		{
			_ibuff.Reserve(_ibuff.Size() + 64 * 1024);
		}
		int read_num = NetOs_Recv(_socket.GetSocket(), _ibuff.End(), _ibuff.Captity() - _ibuff.Size());
		if (read_num > 0)
		{
			_ibuff.Commit((size_t)read_num);
			round += (size_t)read_num;
			continue;
		}
		else if (read_num == -1 && errno == EAGAIN)
		{
			goto read_finish;
		}
		else if (read_num == -1 && errno == EINTR)
		{
			continue;
		}
		if (read_num == 0)
		{
			goto peer_close;
		}
		goto read_fault;
	}
read_finish:
	_p_session->SwapInBuff(_ibuff);
	return;
read_fault:
	_p_session->SwapInBuff(_ibuff);
	_p_session->PassiveClose(errno);
	return;
peer_close:
	_p_session->SwapInBuff(_ibuff);
	_p_session->PassiveClose(0);
	return;
}



void TCP_Channel::DataOut()
{
	_p_session->SwapOutBuff(_obuff);
	while (_obuff.Size())
	{
		int write_num = NetOs_Send(_socket.GetSocket(), _obuff.Begin(), _obuff.Size());
		if (write_num > 0)
		{
			_obuff.Erase(0, write_num);
			continue;
		}
		else if (write_num == -1 && errno == EAGAIN)
		{
			goto write_finish;
		}
		else if (write_num == -1 && errno == EINTR)
		{
			continue;
		}
		goto write_fault;
	}
write_finish:
	if (_obuff.Size() == 0)
	{
		_p_session->SendDrained();
	}
	return;
write_fault:
	_p_session->PassiveClose(errno);
	return;
}
}}