#include "bytes_stream.h"
#include <string>
#include <string.h>
#include "serialize.h"

//暂不处理大小端问题，默认小端
/*
	glibc 2.9
	   #include <endian.h>

	   uint16_t htobe16(uint16_t host_16bits);
	   uint16_t htole16(uint16_t host_16bits);
	   uint16_t be16toh(uint16_t big_endian_16bits);
	   uint16_t le16toh(uint16_t little_endian_16bits);

	   uint32_t htobe32(uint32_t host_32bits);
	   uint32_t htole32(uint32_t host_32bits);
	   uint32_t be32toh(uint32_t big_endian_32bits);
	   uint32_t le32toh(uint32_t little_endian_32bits);

	   uint64_t htobe64(uint64_t host_64bits);
	   uint64_t htole64(uint64_t host_64bits);
	   uint64_t be64toh(uint64_t big_endian_64bits);
	   uint64_t le64toh(uint64_t little_endian_64bits);
*/

BytesStream& BytesStream::operator<< (const std::string& a)
{
	Bytes b(a.c_str(), a.length());
	*this << b;
	return *this;
}
BytesStream& BytesStream::operator>> (std::string& a)
{
	Bytes b;
	*this >> b;
	a = std::string(b.Begin(), b.Size());
	return *this;
}

BytesStream& BytesStream::operator<< (const Bytes& a)
{
	*this << (int)a.Size();
	Append(a);
	return *this;
}
BytesStream& BytesStream::operator>> (Bytes& a)
{
	int size;
	*this >> size;
	if (size < 0 || RemainSize() < (unsigned int)size)
	{
		throw ParserException();
	}
	char* p_a = GetCurPtr();
	a.Append(p_a, size);
	_cur_pos += size;
	return *this;
}


BytesStream& BytesStream::operator<< (const BytesStream& a)
{
	Append(a);
	return *this;
}

BytesStream& BytesStream::operator<< (const Serialize& a)
{
	a.serialize(*this);
	return *this;
}

BytesStream& BytesStream::operator>> (Serialize& a)
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

BytesStream& BytesStream::WritePb(uint32_t seq, const Serialize& value)
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

void BytesStream::ReadPb(uint32_t size, Serialize& value)
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
