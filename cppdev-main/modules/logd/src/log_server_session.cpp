#include "log_server_session.h"
#include "log_protocol.h"

void LogServerSession::OnDataIn()
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
