#pragma once
#include "oi_bytes_stream.h"

namespace OI
{
namespace Rpc
{
class Serialize
{
  public:
	virtual ~Serialize()
	{
	}
	virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& stOutputStream) const = 0;
	virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& stOutputStream) = 0;
	virtual void Encode(::OI::Base::BytesStream& stOutputStream) const
	{
		serialize(stOutputStream);
	}
	virtual bool Decode(::OI::Base::BytesStream& stOutputStream)
	{
		deserialize(stOutputStream);
		return true;
	}

	virtual ::OI::Base::BytesStream& operator<<(::OI::Base::BytesStream& stOutputStream) const
	{
		return serialize(stOutputStream);
	}
	virtual ::OI::Base::BytesStream& operator>>(::OI::Base::BytesStream& stOutputStream)
	{
		return deserialize(stOutputStream);
	}
};

class RpcData : public Serialize
{
  public:
	virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& stOutputStream) const = 0;
	virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& stOutputStream) = 0;
};
}
}
