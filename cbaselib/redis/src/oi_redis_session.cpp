#include "oi_redis_session.h"
#include "oi_redis.h"
namespace OI { namespace Redis {



void RedisClientSession::OnDataIn()
{
	if (stInputBuffer.Size() == 0)
	{
		return;
	}
	RedisClient::GetInstance().Feed(stInputBuffer.Begin(), stInputBuffer.Size());
	stInputBuffer.Clear();
}

void RedisClientSession::OnDataOut()
{
}
}}
