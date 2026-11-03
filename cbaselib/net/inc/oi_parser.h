#pragma once
#include "oi_bytes_stream.h"
#include "oi_thread.h"
#include <string>

namespace OI { namespace Net {

template<class PROTOCOL>
class Parser
{
protected:
	int32_t iParserError;
public:
	Parser():iParserError(PARSER_ERR_SUCCESS){}
	int32_t	GetError()	{	return iParserError;	}
	virtual PROTOCOL* DecodeProtocol(::OI::Base::BytesStream& stOutputStreamParam) = 0;
	virtual ::OI::Base::BytesStream EncodeProtocol(const PROTOCOL* pProtoParam) = 0;
	virtual ::OI::Base::BytesStream EncodeProtocol(const PROTOCOL& stProtoParam) {return EncodeProtocol(&stProtoParam);}
};

class JBytesParser: public Parser<::OI::Base::Bytes>
{
public:
	virtual ::OI::Base::Bytes* DecodeProtocol(::OI::Base::BytesStream& stObuffParam)
	{
		::OI::Base::Bytes* pPointer = new ::OI::Base::Bytes();
		::OI::Base::BytesStream stOutputStream(*pPointer);
		stOutputStream << stObuffParam;
		return pPointer;
	}
	virtual ::OI::Base::BytesStream EncodeProtocol(const ::OI::Base::Bytes* pProtoParam)
	{
		return ::OI::Base::Bytes(*pProtoParam);
	}
};

}}

namespace OI { namespace Rpc {
::OI::Base::BytesStream EncodePbFrame(proto_type_t iIdParam, ::OI::Base::BytesStream& stBodyParam);
}}
