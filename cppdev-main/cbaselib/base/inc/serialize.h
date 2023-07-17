#pragma once
#include "bytes_stream.h"

class Serialize
{
public:
	virtual ~Serialize() {}
	virtual BytesStream& serialize(BytesStream& os)  const = 0;
	virtual BytesStream& deserialize(BytesStream& os) = 0;
	virtual void Encode(BytesStream& os) const
	{
		serialize(os);
	}
	virtual bool Decode(BytesStream& os)
	{
		deserialize(os);
		return true;
	}

	virtual BytesStream& operator<<(BytesStream& os) const
	{
		return serialize(os);
	}
	virtual BytesStream& operator>>(BytesStream& os)
	{
		return deserialize(os);
	}
};
