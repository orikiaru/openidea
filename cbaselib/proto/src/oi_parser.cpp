#include "oi_parser.h"
#include "oi_protocol.h"
#include <map>
#include <assert.h>

Protocol* DecodePbFrame(OI::Base::BytesStream& os, int32_t& parser_err, Protocol* (*get_stub)(proto_type_t))
{
	Protocol* pProtocol = NULL;
	os.StartTransaction();
	do
	{
		uint32_t seq = 0;
		uint32_t size = 0;
		OI::Base::PbHeaderState st = os.PreparePbHeader(seq, size);
		if (st == OI::Base::PB_HEADER_NEED_MORE)
		{
			parser_err = PARSER_ERR_UNFINISH;
			break;
		}
		if (st != OI::Base::PB_HEADER_OK)
		{
			parser_err = PARSER_ERR_OTHER;
			break;
		}

		try
		{
			OI::Base::BytesStream op;
			if (size > 0)
			{
				op.Append(os.GetCurPtr(), size);
			}
			os.AddCurPos(size);

			Protocol* p = get_stub((proto_type_t)seq);
			if (!p)
			{
				parser_err = PARSER_ERR_PROTOCOL_TYPE;
				break;
			}
			pProtocol = p->Clone();
			pProtocol->Decode(op);
			if (op.RemainSize())
			{
				parser_err = PARSER_ERR_PROTOCOL_SIZE;
				SAFE_DELETE(pProtocol);
				break;
			}
		}
		catch (OI::Base::ParserException&)
		{
			parser_err = PARSER_ERR_OTHER;
			SAFE_DELETE(pProtocol);
			break;
		}
	} while (0);

	if (!pProtocol)
	{
		os.RollBack();
	}
	return pProtocol;
}

OI::Base::BytesStream EncodePbFrame(proto_type_t id, OI::Base::BytesStream& body)
{
	if (body.Size() > (size_t)0x7FFFFFFF)
	{
		throw OI::Base::ParserException();
	}
	OI::Base::BytesStream os;
	uint32_t size = (uint32_t)body.Size();
	const void* data = NULL;
	if (size > 0)
	{
		data = body.Begin();
	}
	os.WritePbRaw((uint32_t)id, data, size);
	return os;
}

Protocol* ProtocolParser::DecodeProtocol(OI::Base::BytesStream& os)
{
	(void)os;
	_parser_err = PARSER_ERR_PROTOCOL_TYPE;
	return NULL;
}

OI::Base::BytesStream ProtocolParser::EncodeProtocol(const Protocol* proto)
{
	OI::Base::BytesStream body;
	proto->Encode(body);
	return EncodePbFrame(proto->GetID(), body);
}

