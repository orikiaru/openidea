#ifndef __j_parser_h__
#define __j_parser_h__
#include "bytes_stream.h"
#include "threadpool.h"
#include <string>

class Protocol;
template<class PROTOCOL>
class Parser
{
protected:
	int _parser_err;	//只有DecodeProtocol返回NULL时有效
public:
	Parser():_parser_err(PARSER_ERR_SUCCESS){}
	int	GetError()	{	return _parser_err;	}
	virtual PROTOCOL* DecodeProtocol(BytesStream& os) = 0;
	virtual BytesStream EncodeProtocol(const PROTOCOL* proto) = 0;
	virtual BytesStream EncodeProtocol(const PROTOCOL& proto) {return EncodeProtocol(&proto);}
};

//现有协议结构解码器
class ProtocolParser: public Parser<Protocol>
{
public:
	virtual Protocol* DecodeProtocol(BytesStream& os);
	virtual BytesStream EncodeProtocol(const Protocol* proto);
};
 
//测试用
class JBytesParser: public Parser<Bytes>
{
public:
	virtual Bytes* DecodeProtocol(BytesStream& obuff) 
	{
		Bytes* p = new Bytes();
		BytesStream os(*p);
		os << obuff;
		return p;
	}
	virtual BytesStream EncodeProtocol(const Bytes* proto)
	{
		return Bytes(*proto);
	}
};
#endif
