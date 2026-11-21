#pragma once
#include "oi_bytes_stream.h"
#include "oi_header.h"

namespace OI
{
namespace Rpc
{
::OI::Base::BytesStream EncodePbFrame(proto_type_t iId, ::OI::Base::BytesStream& stBody);
}
}
