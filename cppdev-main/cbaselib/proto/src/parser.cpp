#include "parser.h"
#include "protocol.h"
#include "proto_stub_manager.h"
#include <map>
#include <assert.h>

Protocol* g_GetProtocolStub(int type)
{
	return ProtocolStubs::GetInstance().GetStub(type);
}

Protocol* DecodePbFrame(BytesStream& os, int& parser_err, Protocol* (*get_stub)(proto_type_t))
{
	Protocol* pProtocol = NULL;
	os.StartTransaction();
	do
	{
		uint32_t seq = 0;
		uint32_t size = 0;
		PbHeaderState st = os.PreparePbHeader(seq, size);
		if (st == PB_HEADER_NEED_MORE)
		{
			parser_err = PARSER_ERR_UNFINISH;
			break;
		}
		if (st != PB_HEADER_OK)
		{
			parser_err = PARSER_ERR_OTHER;
			break;
		}

		try
		{
			BytesStream op;
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
		catch (ParserException&)
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

BytesStream EncodePbFrame(proto_type_t id, BytesStream& body)
{
	if (body.Size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	BytesStream os;
	uint32_t size = (uint32_t)body.Size();
	const void* data = NULL;
	if (size > 0)
	{
		data = body.Begin();
	}
	os.WritePbRaw((uint32_t)id, data, size);
	return os;
}

Protocol* ProtocolParser::DecodeProtocol(BytesStream& os)
{
	return DecodePbFrame(os, _parser_err, g_GetProtocolStub);
}

BytesStream ProtocolParser::EncodeProtocol(const Protocol* proto)
{
	BytesStream body;
	proto->Encode(body);
	return EncodePbFrame(proto->GetID(), body);
}

