#include "log_server_session.h"
#include "oi_log_frame.h"
#include "log_manager.h"

void LogServerSession::OnDataIn()
{
	const char* pData = stInputBuffer.Begin();
	size_t iSize = stInputBuffer.Size();
	size_t iUsed = 0;
	while (iSize - iUsed >= (size_t)::OI::Log::LOG_FRAME_HEAD)
	{
		uint32_t iPayload = ::OI::Log::ReadLogFrameLen(pData + iUsed);
		if (iPayload < 1 || iPayload > (uint32_t)::OI::Log::LOG_FRAME_MAX)
		{
			PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			return;
		}
		if (iSize - iUsed < (size_t)::OI::Log::LOG_FRAME_HEAD + iPayload)
		{
			break;
		}
		int iLevel = (unsigned char)pData[iUsed + ::OI::Log::LOG_FRAME_HEAD];
		const char* pText = pData + iUsed + ::OI::Log::LOG_FRAME_HEAD + 1;
		size_t iTextLength = iPayload - 1;
		if (iLevel >= 0 && iLevel < LOG_LEVEL::LOG_LEVEL_COUNT)
		{
			::OI::Base::Bytes stBody(pText, iTextLength);
			LogManager::GetInstance().WriteLog((LOG_LEVEL)iLevel, stBody);
		}
		iUsed += (size_t)::OI::Log::LOG_FRAME_HEAD + iPayload;
	}
	if (iUsed > 0)
	{
		stInputBuffer.Erase(0, iUsed);
	}
}
