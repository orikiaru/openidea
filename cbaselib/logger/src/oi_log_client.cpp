#include "oi_log_client.h"
#include "oi_config.h"
#include "oi_log_frame.h"
#include "string.h"
namespace OI
{
namespace Log
{

LogClient::~LogClient()
{
}

bool LogClient::Init()
{
	LOG_TRACE("LogClient::Init");
	return ::OI::Rpc::ProtoClientManager::Init();
}

void LogClient::OnClientConnect(::OI::Net::Session* pSessionParam)
{
	LOG_TRACE("LogClient::OnClientConnect sid=%d", pSessionParam->GetSessionID());
}

void LogClient::OnClientDisconnect(const ::OI::Net::Session* pSessionParam)
{
	LOG_TRACE("LogClient::OnClientDisconnect sid=%d, errno=%d", pSessionParam->GetSessionID(), pSessionParam->GetError());
}

bool LogClient::SendLog(int iLevelParam, const char* pTextParam, size_t iTextLengthParam)
{
	if (!pTextParam && iTextLengthParam > 0)
	{
		return false;
	}
	if (iTextLengthParam + 1 > (size_t)LOG_FRAME_MAX)
	{
		return false;
	}
	::OI::Base::Bytes stFrame;
	AppendLogFrame(stFrame, iLevelParam, pTextParam, iTextLengthParam);
	// 进程刚起、日志服还没连上时的日志先攒着，别丢。
	const size_t iItemIndexMaxPending = 4 * 1024 * 1024;
	return SendRawOrPend(stFrame, iItemIndexMaxPending);
}
}
}