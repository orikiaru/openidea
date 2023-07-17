#include "proto_session.h"
#include "protocol.h"

void ProtoSession::SendProtocol(const Protocol* proto)
{
	BytesStream os = _parser.EncodeProtocol(proto);
	{
		MutexGuard l(_queue_locker);
		_msg_queue.push_back(os.GetMutableBuff());
	}

	AllowSend();
}

void ProtoSession::OnDataIn()
{
	//TODO decryption
	BytesStream os(_ibuff);
	while (os.RemainSize())
	{
		Protocol* p = _parser.DecodeProtocol(os);
		if (p)
		{
			PTask* p_task = new PTask(p, _p_session_mgr, _session_id);
			ThreadPool::GetInstance()->AddTask(p_task);
			continue;
		}
		else if (_parser.GetError() == PARSER_ERR_UNFINISH)
		{
			break;
		}
		PassiveClose(_parser.GetError());
		break;
	}
	_ibuff.Erase(0, os.GetCurPos());
}

//OnDataOut为有锁环境
void ProtoSession::OnDataOut()
{
	MSG_QUEUE tmp;
	{
		MutexGuard l(_queue_locker);
		tmp.swap(_msg_queue);
	}
	for (MSG_QUEUE::iterator it = tmp.begin(); it != tmp.end(); ++it)
	{
		_obuff.Append(*it);
	}
}
