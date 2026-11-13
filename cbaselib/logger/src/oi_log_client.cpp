#include "oi_log_client.h"
#include "oi_config.h"
#include "oi_log_frame.h"
#include "oi_errno.h"
#include "string.h"
namespace OI
{
namespace Log
{

LogClient::~LogClient()
{
}

int32_t LogClient::Init()
{
	LOG_TRACE("LogClient::Init");
	return ::OI::Rpc::ProtoClientManager::Init();
}

void LogClient::OnClientConnect(::OI::Net::Session* pSession)
{
	LOG_TRACE("LogClient::OnClientConnect sid=%d", pSession->GetSessionID());
}

void LogClient::OnClientDisconnect(const ::OI::Net::Session* pSession)
{
	LOG_TRACE("LogClient::OnClientDisconnect sid=%d, errno=%d", pSession->GetSessionID(), pSession->GetError());
}

int32_t LogClient::SendLog(int iLevel, const char* pText, size_t iTextLength)
{
	if (!pText && iTextLength > 0)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	if (iTextLength >= (size_t)LOG_FRAME_MAX)
	{
		return OI_ERR_INVALID_ARGUMENT;
	}
	::OI::Base::Bytes stFrame;
	AppendLogFrame(stFrame, iLevel, pText, iTextLength);
	// 进程刚起、日志服还没连上时的日志先攒着，别丢。
	const size_t iItemIndexMaxPending = 4 * 1024 * 1024;
	return SendRawOrPend(stFrame, iItemIndexMaxPending);
}
}
}