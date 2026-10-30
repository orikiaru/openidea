#include "oi_parser.h"
namespace OI { namespace Rpc {

::OI::Base::BytesStream EncodePbFrame(proto_type_t id, ::OI::Base::BytesStream& body)
{
	if (body.Size() > (size_t)0x7FFFFFFF)
	{
		throw ::OI::Base::ParserException();
	}
	::OI::Base::BytesStream os;
	uint32_t size = (uint32_t)body.Size();
	const void* data = NULL;
	if (size > 0)
	{
		data = body.Begin();
	}
	os.WritePbRaw((uint32_t)id, data, size);
	return os;
}
}}

