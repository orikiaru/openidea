#include "redis_server_session.h"
#include "redis_server.h"
#include "hiredis.h"

RedisServerSession::RedisServerSession(RedisServer* owner)
	: NetSession(owner), _owner(owner), _reader(redisReaderCreate()), _link(0)
{
}

RedisServerSession::~RedisServerSession()
{
	if (_reader)
	{
		redisReaderFree(_reader);
		_reader = NULL;
	}
}

void RedisServerSession::Send(const Bytes& bytes)
{
	MutexGuard lock(_locker);
	_obuff.Append(bytes);
	AllowSend();
}

void RedisServerSession::SendProtocol(const Protocol* proto)
{
	(void)proto;
	assert(false && "redis server speaks RESP");
}

void RedisServerSession::OnDataOut()
{
}

void RedisServerSession::OnDataIn()
{
	if (!_reader || _ibuff.Size() == 0)
	{
		return;
	}
	if (redisReaderFeed(_reader, _ibuff.Begin(), _ibuff.Size()) != REDIS_OK)
	{
		PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
		return;
	}
	_ibuff.Clear();
	for (;;)
	{
		void* raw = NULL;
		if (redisReaderGetReply(_reader, &raw) != REDIS_OK)
		{
			PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			return;
		}
		if (!raw)
		{
			return;
		}
		redisReply* reply = (redisReply*)raw;
		std::vector<std::string> args;
		if (reply->type == REDIS_REPLY_ARRAY)
		{
			for (size_t i = 0; i < reply->elements; ++i)
			{
				redisReply* part = reply->element[i];
				if (part && part->type == REDIS_REPLY_STRING && part->str)
				{
					args.push_back(std::string(part->str, part->len));
				}
			}
		}
		freeReplyObject(reply);
		if (!args.empty())
		{
			_owner->Issue(_link, GetSessionID(), std::move(args));
		}
	}
}
