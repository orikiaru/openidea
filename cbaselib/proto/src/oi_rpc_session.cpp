#include "oi_rpc_session.h"
#include "oi_rpc_call.h"
namespace OI
{
namespace Rpc
{

void ProtoSession::OnDataIn()
{
	// 一包就是 协议id + 总大小 + 序列化结构。对不上或者解析失败，丢掉这一包。
	// 借 _ibuff 的内存原地解析，不整块拷一份。
	::OI::Base::BytesStream stOutputStream;
	stOutputStream.Swap(stInputBuffer);
	while (stOutputStream.RemainSize())
	{
		stOutputStream.StartTransaction();
		uint32_t iIdLocal = 0;
		uint32_t iSize = 0;
		::OI::Base::PbHeaderState iSt = stOutputStream.PreparePbHeader(iIdLocal, iSize);
		if (iSt == ::OI::Base::PB_HEADER_NEED_MORE)
		{
			stOutputStream.RollBack();
			break;
		}
		if (iSt != ::OI::Base::PB_HEADER_OK)
		{
			PassiveClose(PARSER_ERR_OTHER);
			break;
		}
		::OI::Base::BytesStream stBody;
		if (iSize > 0)
		{
			stBody.Append(stOutputStream.GetCurPtr(), iSize);
		}
		stOutputStream.AddCurPos(iSize);
		DeliverRpc((proto_type_t)iIdLocal, stBody, pSessionManager, iSessionId);
	}
	stInputBuffer.Swap(stOutputStream);
	stInputBuffer.Erase(0, stOutputStream.GetCurPos());
}

void ProtoSession::OnDataOut()
{
}
}
}
