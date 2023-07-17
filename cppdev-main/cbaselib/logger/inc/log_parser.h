#pragma once
#include "parser.h"
class LogProtocol;
//现有协议结构解码器
class LogProtocolParser : public Parser<LogProtocol>
{
public:
	virtual LogProtocol* DecodeProtocol(BytesStream& os);
	virtual BytesStream EncodeProtocol(const LogProtocol* proto);
};