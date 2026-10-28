#include "log_client.h"
#include "config.h"
#include "log_frame.h"
#include "string.h"
LogClient LogClient::inst;

LogClient::~LogClient()
{
}

bool LogClient::Init()
{
	LOG_TRACE("LogClient::Init");
	return ProtoClientManager::Init();
}

void LogClient::OnClientConnect(Session* pSession)
{
	LOG_TRACE("LogClient::OnClientConnect sid=%d", pSession->GetSessionID());
}

void LogClient::OnClientDisconnect(const Session* pSession)
{
	LOG_TRACE("LogClient::OnClientDisconnect Will Reconnect sid=%d, errno=%d", pSession->GetSessionID(), pSession->GetError());
	Reconnect();
}

void LogClient::OnAbortSession()
{
	LOG_TRACE("LogClient::OnAbortSession Will Reconnect");
	Reconnect();
}

bool LogClient::SendLog(int level, const char* text, size_t text_len)
{
	if (!text && text_len > 0)
	{
		return false;
	}
	if (text_len + 1 > (size_t)LOG_FRAME_MAX)
	{
		return false;
	}
	Bytes frame;
	AppendLogFrame(frame, level, text, text_len);
	// 进程刚起、日志服还没连上时的日志先攒着，别丢。
	const size_t kMaxPending = 4 * 1024 * 1024;
	return SendRawOrPend(frame, kMaxPending);
}