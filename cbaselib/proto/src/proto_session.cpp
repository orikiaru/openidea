#include "proto_session.h"
#include "protocol.h"
#include "rpc.h"

void ProtoSession::Send(const Bytes& bytes)
{
	MutexGuard l(_locker);
	_obuff.Append(bytes);
	AllowSend();
}

void ProtoSession::SendProtocol(const Protocol* proto)
{
	BytesStream os = _parser.EncodeProtocol(proto);
	Send(os.GetUnMutableBuff());
}

void ProtoSession::OnDataIn()
{
	// 一包就是 协议id + 总大小 + 序列化结构。对不上或者解析失败，丢掉这一包。
	// 借 _ibuff 的内存原地解析，不整块拷一份。
	BytesStream os;
	os.Swap(_ibuff);
	while (os.RemainSize())
	{
		os.StartTransaction();
		uint32_t id = 0;
		uint32_t size = 0;
		PbHeaderState st = os.PreparePbHeader(id, size);
		if (st == PB_HEADER_NEED_MORE)
		{
			os.RollBack();
			break;
		}
		if (st != PB_HEADER_OK)
		{
			PassiveClose(PARSER_ERR_OTHER);
			break;
		}
		BytesStream body;
		if (size > 0)
		{
			body.Append(os.GetCurPtr(), size);
		}
		os.AddCurPos(size);
		DeliverRpc((proto_type_t)id, body, _p_session_mgr, _session_id);
	}
	_ibuff.Swap(os);
	_ibuff.Erase(0, os.GetCurPos());
}

void ProtoSession::OnDataOut()
{
}
