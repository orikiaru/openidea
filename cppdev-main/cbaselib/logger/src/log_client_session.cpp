#include "log_client_session.h"
#include "log_protocol.h"
LogProtocolParser LogClientSession::_parser;

void LogClientSession::SendProtocol(const LogProtocol* proto)
{
	BytesStream os = _parser.EncodeProtocol(proto);
	MutexGuard l(_locker);
	_obuff.Append(os);
	AllowSend();
}

void LogClientSession::OnDataIn()
{

}

void LogClientSession::OnDataOut()
{
}
