#include "oi_redis_session.h"
#include "oi_redis.h"
namespace OI { namespace Redis {



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
}}
