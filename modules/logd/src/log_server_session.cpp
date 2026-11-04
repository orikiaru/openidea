#include "log_server_session.h"
#include "oi_log_frame.h"
#include "log_manager.h"

void LogServerSession::OnDataIn()
{
	if (m_bWaiting)
	{
		return;
	}
	const char* pData = stInputBuffer.Begin();
	size_t iSize = stInputBuffer.Size();
	size_t iUsed = 0;
	size_t iFrames = 0;
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
		// 每轮最多约 1MiB/1024 帧；超过预算的大帧允许独占一轮，不能切开日志。
		if (iUsed > 0 && (iUsed + ::OI::Log::LOG_FRAME_HEAD + iPayload > 1024 * 1024 || iFrames >= 1024))
		{
			LogManager::GetInstance().DeferWrite(this, iPayload - 1);
			break;
		}
		int iLevel = (unsigned char)pData[iUsed + ::OI::Log::LOG_FRAME_HEAD];
		const char* pText = pData + iUsed + ::OI::Log::LOG_FRAME_HEAD + 1;
		size_t iTextLength = iPayload - 1;
		if (iLevel >= 0 && iLevel < LOG_LEVEL::LOG_LEVEL_COUNT)
		{
			if (!LogManager::GetInstance().WriteLog((LOG_LEVEL)iLevel, pText, iTextLength, this))
			{
				break;
			}
		}
		iUsed += (size_t)::OI::Log::LOG_FRAME_HEAD + iPayload;
		++iFrames;
	}
	if (iUsed > 0)
	{
		stInputBuffer.Erase(0, iUsed);
	}
}

void LogServerSession::WaitForWrite()
{
	m_bWaiting = true;
	if (!m_bShutdown)
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		ForbidRead();
	}
}

void LogServerSession::ResumeWrite(bool bShutdownParam)
{
	m_bShutdown = bShutdownParam;
	m_bWaiting = false;
	// EOF 可能和最后一次读取同时到达。即使已断开，也必须排空已读完整帧。
	OnDataIn();
	if (!m_bWaiting && !m_bShutdown)
	{
		::OI::Thread::MutexGuard stLock(stLocker);
		if (!IsClosed())
		{
			AllowRead();
		}
	}
}
