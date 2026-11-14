#pragma once
#include "oi_bytes_stream.h"

namespace OI
{
namespace Rpc
{
// 协议对象的二进制读写基类。生成结构和手写结构都从这里派生。
class Serialize
{
  public:
	virtual ~Serialize()
	{
	}
	// 把对象写进流。
	virtual ::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& stOutputStream) const = 0;
	// 从流里读回对象。字节不够时由流抛出。
	virtual ::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& stOutputStream) = 0;
	// 编码入口，默认转到 serialize。
	virtual void Encode(::OI::Base::BytesStream& stOutputStream) const
	{
		serialize(stOutputStream);
	}
	// 解码入口，默认转到 deserialize。字节不够时由流抛出。
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

// 生成出来的协议结构的公共基类。必须实现 serialize 和 deserialize。
class RpcData : public Serialize
{
  public:
	::OI::Base::BytesStream& serialize(::OI::Base::BytesStream& stOutputStream) const override = 0;
	::OI::Base::BytesStream& deserialize(::OI::Base::BytesStream& stOutputStream) override = 0;
};
}
}
