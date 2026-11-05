#pragma once
#include "oi_bytes.h"
#include <stdint.h>
#include <string.h>
namespace OI
{
namespace Log
{

// 进程间日志帧，小端：
// uint32 payload_len | uint8 level | payload_len-1 字节正文
// payload_len 至少为 1。单帧正文不超过 8MB。
enum
{
	LOG_FRAME_HEAD = 4,
	LOG_FRAME_MAX = 8 * 1024 * 1024
};

inline void AppendLogFrame(::OI::Base::Bytes& stOutput, int iLevel, const char* pText, size_t iTextLength)
{
	uint32_t iPayload = (uint32_t)(1 + iTextLength);
	uint8_t vHdr[5];
	vHdr[0] = (uint8_t)(iPayload);
	vHdr[1] = (uint8_t)(iPayload >> 8);
	vHdr[2] = (uint8_t)(iPayload >> 16);
	vHdr[3] = (uint8_t)(iPayload >> 24);
	vHdr[4] = (uint8_t)iLevel;
	stOutput.Append(vHdr, 5);
	if (iTextLength > 0 && pText)
	{
		stOutput.Append(pText, iTextLength);
	}
}

inline uint32_t ReadLogFrameLen(const char* pPointer)
{
	const uint8_t* pU = (const uint8_t*)pPointer;
	return (uint32_t)pU[0] | ((uint32_t)pU[1] << 8) | ((uint32_t)pU[2] << 16) | ((uint32_t)pU[3] << 24);
}
}
}
