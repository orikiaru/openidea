#include "log_server_session.h"
#include "framework_protocol/oi_rpcdata_logrecord.h"
#include "oi_bytes_stream.h"
#include "oi_log_frame.h"
#include "oi_proto_id.h"
#include "log_manager.h"

void LogServerSession::OnDataIn()
{
	if (m_bWaiting)
	{
		return;
	}
	::OI::Base::BytesStream stPacket;
	stPacket.Swap(stInputBuffer);
	size_t iUsed = 0;
	size_t iFrames = 0;
	while (stPacket.RemainSize() >= sizeof(uint32_t) * 2)
	{
		const uint32_t iDeclared = ::OI::Base::LoadLittleEndian<uint32_t>(stPacket.GetCurPtr() + sizeof(uint32_t));
		if ((int32_t)iDeclared < 0 || iDeclared > (uint32_t)::OI::Log::LOG_FRAME_MAX + 64)
		{
			PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			return;
		}
		if (stPacket.RemainSize() < sizeof(uint32_t) * 2 + (size_t)iDeclared)
		{
			break;
		}
		const size_t iFrameBytes = sizeof(uint32_t) * 2 + (size_t)iDeclared;
		// 每轮最多约 1MiB/1024 帧；超过预算的大帧允许独占一轮，不能切开日志。
		if (iUsed > 0 && (iUsed + iFrameBytes > 1024 * 1024 || iFrames >= 1024))
		{
			::OI::Base::BytesStream stPeek(stPacket.GetCurPtr() + sizeof(uint32_t) * 2, iDeclared);
			::OI::RpcData::LogRecord stPeekRecord;
			try
			{
				uint64_t iCallId = 0;
				stPeek >> iCallId;
				if (iCallId != 0)
				{
					PassiveClose(PARSER_ERR_OTHER);
					return;
				}
				stPeekRecord.deserialize(stPeek);
			}
			catch (...)
			{
				PassiveClose(PARSER_ERR_OTHER);
				return;
			}
			LogManager::GetInstance().DeferWrite(this, stPeekRecord.sText.size());
			break;
		}
		stPacket.StartTransaction();
		uint32_t iProtocol = 0;
		uint32_t iSize = 0;
		const ::OI::Base::PbHeaderState iState = stPacket.PreparePbHeader(iProtocol, iSize);
		if (iState != ::OI::Base::PB_HEADER_OK || (proto_type_t)iProtocol != ::OI::Base::PROTO_LOG_REMOTE)
		{
			PassiveClose(PARSER_ERR_OTHER);
			return;
		}
		::OI::Base::BytesStream stBody(stPacket.GetCurPtr(), iSize);
		stPacket.AddCurPos(iSize);
		::OI::RpcData::LogRecord stRecord;
		try
		{
			uint64_t iCallId = 0;
			stBody >> iCallId;
			if (iCallId != 0)
			{
				PassiveClose(PARSER_ERR_OTHER);
				return;
			}
			stRecord.deserialize(stBody);
		}
		catch (...)
		{
			PassiveClose(PARSER_ERR_OTHER);
			return;
		}
		if (stRecord.sText.size() >= (size_t)::OI::Log::LOG_FRAME_MAX)
		{
			PassiveClose(PARSER_ERR_PROTOCOL_SIZE);
			return;
		}
		if (stRecord.iLevel >= 0 && stRecord.iLevel < LOG_LEVEL::LOG_LEVEL_COUNT)
		{
			const int32_t iStatus = LogManager::GetInstance().WriteLog((LOG_LEVEL)stRecord.iLevel, stRecord.sText.data(), stRecord.sText.size(), this);
			if (iStatus == OI_ERR_BUSY)
			{
				stPacket.RollBack();
				break;
			}
			if (iStatus != OI_OK)
			{
				PassiveClose(iStatus);
				return;
			}
		}
		iUsed += iFrameBytes;
		++iFrames;
	}
	stInputBuffer.Swap(stPacket);
	stInputBuffer.Erase(0, stPacket.GetCurPos());
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

void LogServerSession::ResumeWrite(bool bShutdown)
{
	m_bShutdown = bShutdown;
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
