#include "redis_client_session.h"
#include "redis_client.h"

void RedisClientSession::Send(const OI::Base::Bytes& bytes)
{
	OI::Thread::MutexGuard lock(_locker);
	_obuff.Append(bytes);
	AllowSend();
}

void RedisClientSession::SendProtocol(const Protocol* proto)
{
	(void)proto;
	assert(false && "redis client sends RESP");
}

void RedisClientSession::OnDataIn()
{
	if (_ibuff.Size() == 0)
	{
		return;
	}
	RedisClient::GetInstance().Feed(_ibuff.Begin(), _ibuff.Size());
	_ibuff.Clear();
}

void RedisClientSession::OnDataOut()
{
}
