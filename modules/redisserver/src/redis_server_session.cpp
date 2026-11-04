#include "redis_server_session.h"
#include "redis_server.h"
#include "oi_log.h"
#include <string>
#include <string_view>

namespace
{
const int64_t kMaxBulk = 512LL * 1024 * 1024;

// 从 p 读一个十进制整数，到 \r\n 为止。返回下一段的起点，数据不够返回 NULL，格式错把 bad 置真。
const char* ReadLine(const char* pPointerParam, const char* pEndParam, int64_t& iValueParam, bool& bBadParam)
{
	bool bNeg = false;
	if (pPointerParam < pEndParam && *pPointerParam == '-')
	{
		bNeg = true;
		++pPointerParam;
	}
	int64_t iValue = 0;
	int iDigits = 0;
	for (; pPointerParam < pEndParam; ++pPointerParam)
	{
		if (*pPointerParam == '\r')
		{
			if (pPointerParam + 1 >= pEndParam)
			{
				return NULL;
			}
			if (pPointerParam[1] != '\n' || iDigits == 0)
			{
				bBadParam = true;
				return NULL;
			}
			iValueParam = bNeg ? -iValue : iValue;
			return pPointerParam + 2;
		}
		if (*pPointerParam < '0' || *pPointerParam > '9' || iDigits >= 18)
		{
			bBadParam = true;
			return NULL;
		}
		iValue = iValue * 10 + (*pPointerParam - '0');
		++iDigits;
	}
	return NULL;
}

// 一条完整的 RESP 命令：*N\r\n 后跟 N 个 $len\r\n<data>\r\n。
// 返回这条命令的字节数；不完整返回 0；格式不对把 bad 置真。
size_t ScanCommand(const char* pBeginParam, const char* pEndParam, int64_t& iArgumentCountParam, bool& bBadParam, size_t iMaxBytesParam)
{
	const char* pPointer = pBeginParam;
	if (pPointer >= pEndParam)
	{
		return 0;
	}
	if (*pPointer != '*')
	{
		bBadParam = true;
		return 0;
	}
	pPointer = ReadLine(pPointer + 1, pEndParam, iArgumentCountParam, bBadParam);
	if (!pPointer)
	{
		return 0;
	}
	if (iArgumentCountParam <= 0)
	{
		bBadParam = true;
		return 0;
	}
	for (int64_t iIndexLocal = 0; iIndexLocal < iArgumentCountParam; ++iIndexLocal)
	{
		if (pPointer >= pEndParam)
		{
			return 0;
		}
		if (*pPointer != '$')
		{
			bBadParam = true;
			return 0;
		}
		int64_t iLength = 0;
		pPointer = ReadLine(pPointer + 1, pEndParam, iLength, bBadParam);
		if (!pPointer)
		{
			return 0;
		}
		if (iLength < 0 || iLength > kMaxBulk)
		{
			bBadParam = true;
			return 0;
		}
		const size_t iPrefixBytes = static_cast<size_t>(pPointer - pBeginParam);
		if (iPrefixBytes > iMaxBytesParam || static_cast<uint64_t>(iLength) + 2 > iMaxBytesParam - iPrefixBytes)
		{
			bBadParam = true;
			return 0;
		}
		if (pEndParam - pPointer < iLength + 2)
		{
			return 0;
		}
		if (pPointer[iLength] != '\r' || pPointer[iLength + 1] != '\n')
		{
			bBadParam = true;
			return 0;
		}
		pPointer += iLength + 2;
	}
	return (size_t)(pPointer - pBeginParam);
}

bool ChangesConnection(const char* pBeginParam, const char* pEndParam)
{
	bool bBad = false;
	int64_t iCount = 0;
	const char* pPointer = ReadLine(pBeginParam + 1, pEndParam, iCount, bBad);
	int64_t iLength = 0;
	pPointer = ReadLine(pPointer + 1, pEndParam, iLength, bBad);
	std::string sCommand(pPointer, (size_t)iLength);
	for (char& iCharacter : sCommand)
	{
		if (iCharacter >= 'a' && iCharacter <= 'z')
		{
			iCharacter = (char)(iCharacter - 'a' + 'A');
		}
	}
	// hiredis async.c 先跳过可选的 P，再按包含 CRLF 的固定前缀识别，忽略命令名长度。
	// PMONITOR 或 MONITOR\r\n<后缀> 即使被 Redis 拒绝，也已改变共享上游的回调状态。
	// 同时覆盖订阅分支；SUBSCRIBE 只有带后续参数时才会改变 hiredis 状态。
	std::string_view sAsyncCommand(sCommand);
	if (sAsyncCommand.starts_with('P'))
	{
		sAsyncCommand.remove_prefix(1);
	}
	if (sAsyncCommand == "MONITOR" || sAsyncCommand.starts_with("MONITOR\r\n") ||
	    sAsyncCommand == "UNSUBSCRIBE" || sAsyncCommand.starts_with("UNSUBSCRIBE\r\n") ||
	    (iCount >= 2 && (sAsyncCommand == "SUBSCRIBE" || sAsyncCommand.starts_with("SUBSCRIBE\r\n"))))
	{
		return true;
	}
	// ScanCommand 已校验完整 RESP；仅检查 SCRIPT 的子命令，保留 LOAD/EXISTS 等正常功能。
	if (sCommand == "SCRIPT" && iCount >= 2)
	{
		pPointer += iLength + 2;
		pPointer = ReadLine(pPointer + 1, pEndParam, iLength, bBad);
		std::string sSubcommand(pPointer, static_cast<size_t>(iLength));
		for (auto& iCharacter : sSubcommand)
		{
			if (iCharacter >= 'a' && iCharacter <= 'z')
			{
				iCharacter = static_cast<char>(iCharacter - 'a' + 'A');
			}
		}
		// Redis 的子命令比较使用 C 字符串，拒绝内嵌 NUL，不能借后缀绕过过滤。
		if (sSubcommand == "DEBUG" || sSubcommand.find('\0') != std::string::npos)
		{
			return true;
		}
	}
	// 上游由多个下游共享，任何连接状态都不能带到其他会话。
	static const char* vCommands[] = {"SELECT", "AUTH", "HELLO", "RESET", "QUIT",
	    "MULTI", "EXEC", "DISCARD", "WATCH", "UNWATCH", "CLIENT", "MONITOR",
	    "SUBSCRIBE", "PSUBSCRIBE", "SSUBSCRIBE", "UNSUBSCRIBE", "PUNSUBSCRIBE",
	    "SUNSUBSCRIBE", "READONLY", "READWRITE", "ASKING", "SYNC", "PSYNC", "REPLCONF"};
	for (const char* pName : vCommands)
	{
		if (sCommand == pName)
		{
			return true;
		}
	}
	return false;
}
}

RedisServerSession::RedisServerSession(RedisServer* pOwnerParam)
    : ::OI::Net::NetSession(pOwnerParam), m_pOwner(pOwnerParam), m_iLink(0)
{
}

RedisServerSession::~RedisServerSession()
{
}

void RedisServerSession::OnDataOut()
{
}

bool RedisServerSession::ResumeRead()
{
	if (IsClosed())
	{
		return false;
	}
	m_bReadPaused = false;
	// 内核可能已经没数据；必须先消费会话中留下的完整命令。
	const size_t iConsumed = ProcessInput(1);
	if (!m_bReadPaused && !IsClosed())
	{
		AllowRead();
	}
	return iConsumed != 0;
}

void RedisServerSession::OnDataIn()
{
	ProcessInput(32);
}

size_t RedisServerSession::ProcessInput(size_t iBudgetParam)
{
	if (m_bReadPaused || IsClosed())
	{
		return 0;
	}
	// 只切出完整命令的边界，原样转给上游，不拆成参数再编一遍。
	const char* pBegin = stInputBuffer.Begin();
	const char* pEnd = pBegin + stInputBuffer.Size();
	const char* pPointer = pBegin;
	while (pPointer < pEnd)
	{
		int64_t iArgumentCount = 0;
		bool bBad = false;
		size_t iNumber = ScanCommand(pPointer, pEnd, iArgumentCount, bBad, m_pOwner->MaxCommandBytes());
		if (bBad)
		{
			PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			break;
		}
		if (iNumber == 0)
		{
			break;
		}
		if (iArgumentCount > 0)
		{
			if (ChangesConnection(pPointer, pPointer + iNumber))
			{
				// 直接断开，不能插入一个本地错误回包，抢在此前未完成的回复前面。
				LOG_ERROR("redisserver rejects connection-state command sid=%d", GetSessionID());
				PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
				break;
			}
			if (!m_pOwner->Issue(m_iLink, GetSessionID(), pPointer, iNumber))
			{
				m_bReadPaused = true;
				ForbidRead();
				m_pOwner->Pause(m_iLink, GetSessionID());
				break;
			}
			if (IsClosed())
			{
				break;
			}
		}
		pPointer += iNumber;
		if (--iBudgetParam == 0 && pPointer < pEnd)
		{
			m_bReadPaused = true;
			ForbidRead();
			m_pOwner->Pause(m_iLink, GetSessionID());
			break;
		}
	}
	const size_t iConsumed = static_cast<size_t>(pPointer - pBegin);
	stInputBuffer.Erase(0, iConsumed);
	return iConsumed;
}
