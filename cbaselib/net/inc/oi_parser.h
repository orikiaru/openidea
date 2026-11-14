#pragma once
#include "oi_bytes_stream.h"
#include "oi_thread.h"
#include <string>

namespace OI
{
namespace Net
{

template <class PROTOCOL>
class Parser
{
  protected:
	int32_t iParserError;

  public:
	Parser()
	    : iParserError(PARSER_ERR_SUCCESS)
	{
	}
	int32_t GetError()
	{
		return iParserError;
	}
	virtual PROTOCOL* DecodeProtocol(::OI::Base::BytesStream& stOutputStream) = 0;
	virtual ::OI::Base::BytesStream EncodeProtocol(const PROTOCOL* pProto) = 0;
	virtual ::OI::Base::BytesStream EncodeProtocol(const PROTOCOL& stProto)
	{
		return EncodeProtocol(&stProto);
	}
};

class JBytesParser : public Parser<::OI::Base::Bytes>
{
  public:
	::OI::Base::Bytes* DecodeProtocol(::OI::Base::BytesStream& stObuff) override
	{
		::OI::Base::Bytes* pPointer = new ::OI::Base::Bytes();
		::OI::Base::BytesStream stOutputStream(*pPointer);
		stOutputStream << stObuff;
		return pPointer;
	}
	::OI::Base::BytesStream EncodeProtocol(const ::OI::Base::Bytes* pProto) override
	{
		return ::OI::Base::Bytes(*pProto);
	}
};

}
}

namespace OI
{
namespace Rpc
{
::OI::Base::BytesStream EncodePbFrame(proto_type_t iId, ::OI::Base::BytesStream& stBody);
}
}
