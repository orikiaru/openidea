#include "log_parser.h"
#include "assert.h"
#include "log_proto_stubs.h"
LogProtocol* LogProtocolParser::DecodeProtocol(BytesStream& os)
{
	Protocol* pProtocol = NULL;
	os.StartTransaction();
	do
	{
		proto_type_t id;
		int size;
		//先解头
		try
		{
			os >> id >> size;
		}
		catch (ParserException & e)
		{
			_parser_err = PARSER_ERR_UNFINISH;
			break;
		}

		if (size > 0 && id >= 0)
		{
			if (os.RemainSize() >= (unsigned int)size)
			{
				//再解协议体
				try
				{
					BytesStream op(os.GetCurPtr(), size);	//浅拷贝
					os.AddCurPos(size);				//消费

					Protocol* p = LogProtoStubs::GetInstance().GetStub(id);
					if (!p)
					{
						_parser_err = PARSER_ERR_PROTOCOL_TYPE;
						break;
					}
					pProtocol = p->Clone();
					pProtocol->Decode(op);

				}
				catch (ParserException & e)
				{
					_parser_err = PARSER_ERR_OTHER;
					SAFE_DELETE(pProtocol);
					break;
				}
			}
			else
			{
				//协议体大小不足,继续接收
				_parser_err = PARSER_ERR_UNFINISH;
				break;
			}
		}
		else
		{
			//其他问题
			_parser_err = PARSER_ERR_OTHER;
			break;
		}
	} while (0);

	if (!pProtocol)
	{
		os.RollBack();	//没解析成功  恢复到原位置
	}
	return dynamic_cast<LogProtocol*>(pProtocol);
}

BytesStream LogProtocolParser::EncodeProtocol(const LogProtocol* proto)
{
	proto_type_t id = proto->GetID();
	BytesStream os, os2;
	proto->Encode(os2);
	os << id;
	os << (int)os2.Size();
	os << os2;
	return os;
}
