#ifndef __j_parser_h__
#define __j_parser_h__
#include "oi_bytes_stream.h"
#include "oi_thread.h"
#include <string>

class Protocol;
template<class PROTOCOL>
class Parser
{
protected:
	int32_t _parser_err;	//只有DecodeProtocol返回NULL时有效
public:
	Parser():_parser_err(PARSER_ERR_SUCCESS){}
	int32_t	GetError()	{	return _parser_err;	}
	virtual PROTOCOL* DecodeProtocol(OI::Base::BytesStream& os) = 0;
	virtual OI::Base::BytesStream EncodeProtocol(const PROTOCOL* proto) = 0;
	virtual OI::Base::BytesStream EncodeProtocol(const PROTOCOL& proto) {return EncodeProtocol(&proto);}
};

// 整包：协议id + totalsize + (seqid + size + data) 重复。seqid 允许缺号。
Protocol* DecodePbFrame(OI::Base::BytesStream& os, int32_t& parser_err, Protocol* (*get_stub)(proto_type_t));
OI::Base::BytesStream EncodePbFrame(proto_type_t id, OI::Base::BytesStream& body);

//现有协议结构解码器
class ProtocolParser: public Parser<Protocol>
{
public:
	virtual Protocol* DecodeProtocol(OI::Base::BytesStream& os);
	virtual OI::Base::BytesStream EncodeProtocol(const Protocol* proto);
};
 
//测试用
class JBytesParser: public Parser<OI::Base::Bytes>
{
public:
	virtual OI::Base::Bytes* DecodeProtocol(OI::Base::BytesStream& obuff) 
	{
		OI::Base::Bytes* p = new OI::Base::Bytes();
		OI::Base::BytesStream os(*p);
		os << obuff;
		return p;
	}
	virtual OI::Base::BytesStream EncodeProtocol(const OI::Base::Bytes* proto)
	{
		return OI::Base::Bytes(*proto);
	}
};
#endif
