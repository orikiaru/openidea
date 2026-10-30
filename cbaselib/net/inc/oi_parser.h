#ifndef __j_parser_h__
#define __j_parser_h__
#include "oi_bytes_stream.h"
#include "oi_thread.h"
#include <string>

namespace OI { namespace Net {

template<class PROTOCOL>
class Parser
{
protected:
	int32_t _parser_err;
public:
	Parser():_parser_err(PARSER_ERR_SUCCESS){}
	int32_t	GetError()	{	return _parser_err;	}
	virtual PROTOCOL* DecodeProtocol(::OI::Base::BytesStream& os) = 0;
	virtual ::OI::Base::BytesStream EncodeProtocol(const PROTOCOL* proto) = 0;
	virtual ::OI::Base::BytesStream EncodeProtocol(const PROTOCOL& proto) {return EncodeProtocol(&proto);}
};

class JBytesParser: public Parser<::OI::Base::Bytes>
{
public:
	virtual ::OI::Base::Bytes* DecodeProtocol(::OI::Base::BytesStream& obuff)
	{
		::OI::Base::Bytes* p = new ::OI::Base::Bytes();
		::OI::Base::BytesStream os(*p);
		os << obuff;
		return p;
	}
	virtual ::OI::Base::BytesStream EncodeProtocol(const ::OI::Base::Bytes* proto)
	{
		return ::OI::Base::Bytes(*proto);
	}
};

}}

namespace OI { namespace Rpc {
::OI::Base::BytesStream EncodePbFrame(proto_type_t id, ::OI::Base::BytesStream& body);
}}
#endif
