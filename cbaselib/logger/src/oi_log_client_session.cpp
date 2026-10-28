#include "oi_log_client_session.h"
namespace OI { namespace Log {

void LogClientSession::Send(const ::OI::Base::Bytes& bytes)
{
	::OI::Thread::MutexGuard l(_locker);
	_obuff.Append(bytes);
	AllowSend();
}

void LogClientSession::OnDataIn()
{

}

void LogClientSession::OnDataOut()
{
}
}}
