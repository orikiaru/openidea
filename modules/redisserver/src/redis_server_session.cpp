#include "redis_server_session.h"
#include "redis_server.h"
#include "oi_log.h"
#include <string>

namespace
{
const int64_t kMaxArgs = 1024 * 1024;
const int64_t kMaxBulk = 512LL * 1024 * 1024;

// 从 p 读一个十进制整数，到 \r\n 为止。返回下一段的起点，数据不够返回 NULL，格式错把 bad 置真。
const char* ReadLine(const char* p, const char* end, int64_t& value, bool& bad)
{
	bool neg = false;
	if (p < end && *p == '-')
	{
		neg = true;
		++p;
	}
	int64_t v = 0;
	int digits = 0;
	for (; p < end; ++p)
	{
		if (*p == '\r')
		{
			if (p + 1 >= end)
			{
				return NULL;
			}
			if (p[1] != '\n' || digits == 0)
			{
				bad = true;
				return NULL;
			}
			value = neg ? -v : v;
			return p + 2;
		}
		if (*p < '0' || *p > '9' || digits >= 18)
		{
			bad = true;
			return NULL;
		}
		v = v * 10 + (*p - '0');
		++digits;
	}
	return NULL;
}

// 一条完整的 RESP 命令：*N\r\n 后跟 N 个 $len\r\n<data>\r\n。
// 返回这条命令的字节数；不完整返回 0；格式不对把 bad 置真。
size_t ScanCommand(const char* begin, const char* end, int64_t& argc, bool& bad)
{
	const char* p = begin;
	if (p >= end)
	{
		return 0;
	}
	if (*p != '*')
	{
		bad = true;
		return 0;
	}
	p = ReadLine(p + 1, end, argc, bad);
	if (!p)
	{
		return 0;
	}
	if (argc <= 0 || argc > kMaxArgs)
	{
		bad = true;
		return 0;
	}
	for (int64_t i = 0; i < argc; ++i)
	{
		if (p >= end)
		{
			return 0;
		}
		if (*p != '$')
		{
			bad = true;
			return 0;
		}
		int64_t len = 0;
		p = ReadLine(p + 1, end, len, bad);
		if (!p)
		{
			return 0;
		}
		if (len < 0 || len > kMaxBulk)
		{
			bad = true;
			return 0;
		}
		if (end - p < len + 2)
		{
			return 0;
		}
		if (p[len] != '\r' || p[len + 1] != '\n')
		{
			bad = true;
			return 0;
		}
		p += len + 2;
	}
	return (size_t)(p - begin);
}

bool ChangesConnection(const char* begin, const char* end)
{
	bool bad = false;
	int64_t count = 0;
	const char* p = ReadLine(begin + 1, end, count, bad);
	int64_t len = 0;
	p = ReadLine(p + 1, end, len, bad);
	std::string command(p, (size_t)len);
	for (char& c : command)
	{
		if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
	}
	// 上游由多个下游共享，任何连接状态都不能带到其他会话。
	static const char* commands[] = {"SELECT", "AUTH", "HELLO", "RESET", "QUIT",
		"MULTI", "EXEC", "DISCARD", "WATCH", "UNWATCH", "CLIENT", "MONITOR",
		"SUBSCRIBE", "PSUBSCRIBE", "SSUBSCRIBE", "UNSUBSCRIBE", "PUNSUBSCRIBE",
		"SUNSUBSCRIBE", "READONLY", "READWRITE", "ASKING", "SYNC", "PSYNC", "REPLCONF"};
	for (const char* name : commands)
	{
		if (command == name) return true;
	}
	return false;
}
}

RedisServerSession::RedisServerSession(RedisServer* owner)
	: ::OI::Net::NetSession(owner), _owner(owner), _link(0)
{
}

RedisServerSession::~RedisServerSession()
{
}

void RedisServerSession::Send(const ::OI::Base::Bytes& bytes)
{
	::OI::Thread::MutexGuard lock(_locker);
	_obuff.Append(bytes);
	AllowSend();
}

void RedisServerSession::SendProtocol(const ::OI::Rpc::Protocol* proto)
{
	(void)proto;
	assert(false && "redis server speaks RESP");
}

void RedisServerSession::OnDataOut()
{
}

void RedisServerSession::OnDataIn()
{
	// 只切出完整命令的边界，原样转给上游，不拆成参数再编一遍。
	const char* begin = _ibuff.Begin();
	const char* end = begin + _ibuff.Size();
	const char* p = begin;
	while (p < end)
	{
		int64_t argc = 0;
		bool bad = false;
		size_t n = ScanCommand(p, end, argc, bad);
		if (bad)
		{
			PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			break;
		}
		if (n == 0)
		{
			break;
		}
		if (argc > 0)
		{
			if (ChangesConnection(p, p + n))
			{
				// 直接断开，不能插入一个本地错误回包，抢在此前未完成的回复前面。
				LOG_ERROR("redisserver rejects connection-state command sid=%d", GetSessionID());
				PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
				break;
			}
			_owner->Issue(_link, GetSessionID(), p, n);
		}
		p += n;
	}
	_ibuff.Erase(0, (size_t)(p - begin));
}
