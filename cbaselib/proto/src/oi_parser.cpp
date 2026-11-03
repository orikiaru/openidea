#include "oi_parser.h"
namespace OI
{
namespace Rpc
{

::OI::Base::BytesStream EncodePbFrame(proto_type_t iIdParam, ::OI::Base::BytesStream& stBodyParam)
{
	if (stBodyParam.Size() > (size_t)0x7FFFFFFF)
	{
		throw ::OI::Base::ParserException();
	}
	::OI::Base::BytesStream stOutputStream;
	uint32_t iSize = (uint32_t)stBodyParam.Size();
	const void* pData = NULL;
	if (iSize > 0)
	{
		pData = stBodyParam.Begin();
	}
	stOutputStream.WritePbRaw((uint32_t)iIdParam, pData, iSize);
	return stOutputStream;
}
}
}
