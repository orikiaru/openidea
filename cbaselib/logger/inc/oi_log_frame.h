#pragma once
#include "framework_protocol/oi_rpcdata_logrecord.h"
#include "oi_bytes.h"
#include "oi_bytes_stream.h"
#include "oi_parser.h"
#include "oi_proto_id.h"
#include <stdint.h>
namespace OI
{
namespace Log
{

// 单条日志正文不超过 8MB。
enum
{
	LOG_FRAME_MAX = 8 * 1024 * 1024
};

inline void AppendLogFrame(::OI::Base::Bytes& stOutput, int iLevel, const char* pText, size_t iTextLength)
{
	::OI::RpcData::LogRecord stRecord;
	stRecord.iLevel = iLevel;
	if (iTextLength > 0 && pText)
	{
		stRecord.sText.assign(pText, iTextLength);
	}
	::OI::Base::BytesStream stBody;
	// 标准 RPC 单向消息帧。
	stBody << uint64_t(0);
	stRecord.serialize(stBody);
	::OI::Base::BytesStream stFrame = ::OI::Rpc::EncodePbFrame(::OI::Base::PROTO_LOG_REMOTE, stBody);
	stOutput.Append(stFrame.Begin(), stFrame.Size());
}
}
}
