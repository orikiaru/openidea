#include "log_client_session.h"

void LogClientSession::Send(const Bytes& bytes)
{
	MutexGuard l(_locker);
	_obuff.Append(bytes);
	AllowSend();
}

void LogClientSession::OnDataIn()
{

}

void LogClientSession::OnDataOut()
{
}
