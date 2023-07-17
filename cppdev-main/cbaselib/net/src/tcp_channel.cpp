#include <unistd.h>
#include "tcp_channel.h"
#include "session.h"

void TCP_Channel::DataIn()
{
	Bytes tmp_bytes;
	tmp_bytes.Reserve(8192);
	for (;;)
	{
		int read_num = read(_socket.GetSocket(), tmp_bytes.Begin(), tmp_bytes.Captity());
		if (read_num > 0)
		{
			_ibuff.Append(tmp_bytes.Begin(), read_num);
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
		int write_num = write(_socket.GetSocket(), _obuff.Begin(), _obuff.Size());
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
		ForbidSend();	//no data unregister writeevent;
	}
	return;
write_fault:
	_p_session->PassiveClose(errno);
	return;
}