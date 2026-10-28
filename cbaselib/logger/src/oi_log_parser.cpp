#include "oi_log_parser.h"
#include "assert.h"

LogProtocol* LogProtocolParser::DecodeProtocol(OI::Base::BytesStream& os)
{
	(void)os;
	_parser_err = PARSER_ERR_PROTOCOL_TYPE;
	return NULL;
}

OI::Base::BytesStream LogProtocolParser::EncodeProtocol(const LogProtocol* proto)
{
	OI::Base::BytesStream body;
	proto->Encode(body);
	return EncodePbFrame(proto->GetID(), body);
}
