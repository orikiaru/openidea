#include "oi_parser.h"
namespace OI
{
namespace Rpc
{

::OI::Base::BytesStream EncodePbFrame(proto_type_t iId, ::OI::Base::BytesStream& stBody)
{
	if (stBody.Size() > (size_t)0x7FFFFFFF)
	{
		throw ::OI::Base::ParserException();
	}
	::OI::Base::BytesStream stOutputStream;
	uint32_t iSize = (uint32_t)stBody.Size();
	const void* pData = NULL;
	if (iSize > 0)
	{
		pData = stBody.Begin();
	}
	stOutputStream.WritePbRaw((uint32_t)iId, pData, iSize);
	return stOutputStream;
}
}
}
