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
	virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& stOutputStreamParam) const = 0;
	virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& stOutputStreamParam) = 0;
	virtual void Encode(::OI::Base::BytesStream& stOutputStreamParam) const
	{
		serialize(stOutputStreamParam);
	}
	virtual bool Decode(::OI::Base::BytesStream& stOutputStreamParam)
	{
		deserialize(stOutputStreamParam);
		return true;
	}

	virtual ::OI::Base::BytesStream& operator<<(::OI::Base::BytesStream& stOutputStreamParam) const
	{
		return serialize(stOutputStreamParam);
	}
	virtual ::OI::Base::BytesStream& operator>>(::OI::Base::BytesStream& stOutputStreamParam)
	{
		return deserialize(stOutputStreamParam);
	}
};

class RpcData : public Serialize
{
  public:
	virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& stOutputStreamParam) const = 0;
	virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& stOutputStreamParam) = 0;
};
}
}
