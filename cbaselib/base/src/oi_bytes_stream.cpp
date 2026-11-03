#include "oi_bytes_stream.h"
#include <string>
#include <string.h>
#include "oi_rpc.h"

namespace OI { namespace Base {
//暂不处理大小端问题，默认小端

// 长度前缀是 4 字节，最高位为 1 的长度对端按非法处理。
static void WriteLenPrefixed(BytesStream& os, const void* data, size_t size)
{
	if (size > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	os << (int32_t)size;
	if (size > 0)
	{
		os.Append(data, size);
	}
}

static size_t ReadLenPrefix(BytesStream& os)
{
	int32_t size;
	os >> size;
	if (size < 0 || os.RemainSize() < (size_t)size)
	{
		throw ParserException();
	}
	return (size_t)size;
}

BytesStream& BytesStream::operator<< (const std::string& a)
{
	WriteLenPrefixed(*this, a.data(), a.size());
	return *this;
}
BytesStream& BytesStream::operator>> (std::string& a)
{
	size_t size = ReadLenPrefix(*this);
	a.assign(GetCurPtr(), size);
	_cur_pos += size;
	return *this;
}

BytesStream& BytesStream::operator<< (const Bytes& a)
{
	WriteLenPrefixed(*this, a.Data(), a.Size());
	return *this;
}
// 覆盖 a 原有内容，和 ReadPb 一致。
BytesStream& BytesStream::operator>> (Bytes& a)
{
	size_t size = ReadLenPrefix(*this);
	a.Clear();
	a.Append(GetCurPtr(), size);
	_cur_pos += size;
	return *this;
}


BytesStream& BytesStream::operator<< (const BytesStream& a)
{
	Append(a);
	return *this;
}

BytesStream& BytesStream::operator<< (const ::OI::Rpc::Serialize& a)
{
	a.serialize(*this);
	return *this;
}

BytesStream& BytesStream::operator>> (::OI::Rpc::Serialize& a)
{
	a.deserialize(*this);
	return *this;
}

BytesStream& BytesStream::WritePbRaw(uint32_t seq, const void* data, uint32_t size)
{
	if ((int32_t)seq < 0 || (int32_t)size < 0)
	{
		throw ParserException();
	}
	Append(&seq, sizeof(seq));
	Append(&size, sizeof(size));
	if (size > 0)
	{
		if (!data)
		{
			throw ParserException();
		}
		Append(data, size);
	}
	return *this;
}

PbHeaderState BytesStream::PreparePbHeader(uint32_t& seq, uint32_t& size)
{
	if (RemainSize() < sizeof(uint32_t) * 2)
	{
		return PB_HEADER_NEED_MORE;
	}
	uint32_t seq_v = 0;
	uint32_t size_v = 0;
	memcpy(&seq_v, GetCurPtr(), sizeof(seq_v));
	memcpy(&size_v, GetCurPtr() + sizeof(seq_v), sizeof(size_v));
	if ((int32_t)seq_v < 0 || (int32_t)size_v < 0)
	{
		return PB_HEADER_BAD;
	}
	if (RemainSize() < sizeof(uint32_t) * 2 + (size_t)size_v)
	{
		return PB_HEADER_NEED_MORE;
	}
	_cur_pos += sizeof(uint32_t) * 2;
	seq = seq_v;
	size = size_v;
	return PB_HEADER_OK;
}

void BytesStream::ReadPbHeader(uint32_t& seq, uint32_t& size)
{
	if (PreparePbHeader(seq, size) != PB_HEADER_OK)
	{
		throw ParserException();
	}
}

void BytesStream::SkipPb(uint32_t size)
{
	if (RemainSize() < size)
	{
		throw ParserException();
	}
	_cur_pos += size;
}

BytesStream& BytesStream::WritePb(uint32_t seq, const std::string& value)
{
	if (value.size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	const void* data = value.empty() ? NULL : value.data();
	return WritePbRaw(seq, data, (uint32_t)value.size());
}

BytesStream& BytesStream::WritePb(uint32_t seq, const Bytes& value)
{
	if (value.Size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	uint32_t size = (uint32_t)value.Size();
	const void* data = size ? value.Data() : NULL;
	return WritePbRaw(seq, data, size);
}

BytesStream& BytesStream::WritePb(uint32_t seq, const ::OI::Rpc::Serialize& value)
{
	BytesStream inner;
	value.serialize(inner);
	if (inner.Size() > (size_t)0x7FFFFFFF)
	{
		throw ParserException();
	}
	uint32_t size = (uint32_t)inner.Size();
	const void* data = NULL;
	if (size > 0)
	{
		data = inner.Begin();
	}
	return WritePbRaw(seq, data, size);
}

void BytesStream::ReadPb(uint32_t size, std::string& value)
{
	if (RemainSize() < size)
	{
		throw ParserException();
	}
	if (size == 0)
	{
		value.clear();
	}
	else
	{
		value.assign(GetCurPtr(), size);
	}
	_cur_pos += size;
}

void BytesStream::ReadPb(uint32_t size, Bytes& value)
{
	if (RemainSize() < size)
	{
		throw ParserException();
	}
	value.Clear();
	if (size > 0)
	{
		value.Append(GetCurPtr(), size);
	}
	_cur_pos += size;
}

void BytesStream::ReadPb(uint32_t size, ::OI::Rpc::Serialize& value)
{
	if (RemainSize() < size)
	{
		throw ParserException();
	}
	BytesStream sub;
	if (size > 0)
	{
		sub.Append(GetCurPtr(), size);
	}
	value.deserialize(sub);
	if (sub.RemainSize() != 0)
	{
		throw ParserException();
	}
	_cur_pos += size;
}
}}
