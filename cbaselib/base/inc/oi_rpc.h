#pragma once
#include "oi_bytes_stream.h"

namespace OI { namespace Rpc {
class Serialize
{
public:
	virtual ~Serialize() {}
	virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os)  const = 0;
	virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os) = 0;
	virtual void Encode(::OI::Base::BytesStream& os) const
	{
		serialize(os);
	}
	virtual bool Decode(::OI::Base::BytesStream& os)
	{
		deserialize(os);
		return true;
	}

	virtual ::OI::Base::BytesStream& operator<<(::OI::Base::BytesStream& os) const
	{
		return serialize(os);
	}
	virtual ::OI::Base::BytesStream& operator>>(::OI::Base::BytesStream& os)
	{
		return deserialize(os);
	}
};

class RpcData : public Serialize
{
public:
	virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& os) const = 0;
	virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& os) = 0;
};
}}
