#include "redis_server_session.h"
#include "redis_server.h"

namespace
{
const long long kMaxArgs = 1024 * 1024;
const long long kMaxBulk = 512LL * 1024 * 1024;

// 从 p 读一个十进制整数，到 \r\n 为止。返回下一段的起点，数据不够返回 NULL，格式错把 bad 置真。
const char* ReadLine(const char* p, const char* end, long long& value, bool& bad)
{
	bool neg = false;
	if (p < end && *p == '-')
	{
		neg = true;
		++p;
	}
	long long v = 0;
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
size_t ScanCommand(const char* begin, const char* end, long long& argc, bool& bad)
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
	if (argc > kMaxArgs)
	{
		bad = true;
		return 0;
	}
	for (long long i = 0; i < argc; ++i)
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
		long long len = 0;
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
}

RedisServerSession::RedisServerSession(RedisServer* owner)
	: NetSession(owner), _owner(owner), _link(0)
{
}

RedisServerSession::~RedisServerSession()
{
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
	// 只切出完整命令的边界，原样转给上游，不拆成参数再编一遍。
	const char* begin = _ibuff.Begin();
	const char* end = begin + _ibuff.Size();
	const char* p = begin;
	while (p < end)
	{
		long long argc = 0;
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
			_owner->Issue(_link, GetSessionID(), p, n);
		}
		p += n;
	}
	_ibuff.Erase(0, (size_t)(p - begin));
}
