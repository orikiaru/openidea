#pragma once
#include "oi_bytes.h"
#include <stdint.h>
#include <string.h>

// 进程间日志帧，小端：
// uint32 payload_len | uint8 level | payload_len-1 字节正文
// payload_len 至少为 1。单帧正文不超过 8MB。
enum
{
	LOG_FRAME_HEAD = 4,
	LOG_FRAME_MAX = 8 * 1024 * 1024
};

inline void AppendLogFrame(OI::Base::Bytes& out, int level, const char* text, size_t text_len)
{
	uint32_t payload = (uint32_t)(1 + text_len);
	uint8_t hdr[5];
	hdr[0] = (uint8_t)(payload);
	hdr[1] = (uint8_t)(payload >> 8);
	hdr[2] = (uint8_t)(payload >> 16);
	hdr[3] = (uint8_t)(payload >> 24);
	hdr[4] = (uint8_t)level;
	out.Append(hdr, 5);
	if (text_len > 0 && text)
	{
		out.Append(text, text_len);
	}
}

inline uint32_t ReadLogFrameLen(const char* p)
{
	const uint8_t* u = (const uint8_t*)p;
	return (uint32_t)u[0] | ((uint32_t)u[1] << 8) | ((uint32_t)u[2] << 16) | ((uint32_t)u[3] << 24);
}
