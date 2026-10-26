#include "log_parser.h"
#include "assert.h"
#include "log_proto_stubs.h"
static Protocol* GetLogStub(proto_type_t id)
{
	return LogProtoStubs::GetInstance().GetStub(id);
}

LogProtocol* LogProtocolParser::DecodeProtocol(BytesStream& os)
{
	Protocol* pProtocol = DecodePbFrame(os, _parser_err, GetLogStub);
	LogProtocol* log_proto = dynamic_cast<LogProtocol*>(pProtocol);
	if (pProtocol && !log_proto)
	{
		SAFE_DELETE(pProtocol);
	}
	return log_proto;
}

BytesStream LogProtocolParser::EncodeProtocol(const LogProtocol* proto)
{
	BytesStream body;
	proto->Encode(body);
	return EncodePbFrame(proto->GetID(), body);
}
