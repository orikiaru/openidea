#pragma once
#include "oi_parser.h"
namespace OI { namespace Log {
class LogProtocol;
//现有协议结构解码器
class LogProtocolParser : public Parser<LogProtocol>
{
public:
	virtual LogProtocol* DecodeProtocol(::OI::Base::BytesStream& os);
	virtual ::OI::Base::BytesStream EncodeProtocol(const LogProtocol* proto);
};
}}