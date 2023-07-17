#include "bytes_stream.h"
#include <string>
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

//暂时先不压缩,省着出问题
BytesStream& BytesStream::operator<< (const char& a)
{
	Append(&a, sizeof(a));
	return *this;
}
BytesStream& BytesStream::operator>> (char& a)
{
	if (RemainSize() < sizeof(a))
	{
		throw ParserException();
	}
	char* p_a = GetCurPtr();
	a = *(char*)(p_a);
	_cur_pos += sizeof(a);
	return *this;
}

BytesStream& BytesStream::operator<< (const short& a)
{
	Append(&a, sizeof(a));
	return *this;
}
BytesStream& BytesStream::operator>> (short& a)
{
	if (RemainSize() < sizeof(a))
	{
		throw ParserException();
	}
	char* p_a = GetCurPtr();
	a = *(short*)(p_a);
	_cur_pos += sizeof(a);
	return *this;
}

BytesStream& BytesStream::operator<< (const int& a)
{
	Append(&a, sizeof(a));
	return *this;
}
BytesStream& BytesStream::operator>> (int& a)
{
	if (RemainSize() < sizeof(a))
	{
		throw ParserException();
	}
	char* p_a = GetCurPtr();
	a = *(int*)(p_a);
	_cur_pos += sizeof(a);
	return *this;
}

BytesStream& BytesStream::operator<< (const uint32_t& a)
{
	Append(&a, sizeof(a));
	return *this;
}

BytesStream& BytesStream::operator>> (uint32_t & a)
{
	if (RemainSize() < sizeof(a))
	{
		throw ParserException();
	}
	char* p_a = GetCurPtr();
	a = *(int*)(p_a);
	_cur_pos += sizeof(a);
	return *this;
}

BytesStream& BytesStream::operator<< (const int64_t& a)
{
	Append(&a, sizeof(a));
	return *this;
}
BytesStream& BytesStream::operator>> (int64_t& a)
{
	if (RemainSize() < sizeof(a))
	{
		throw ParserException();
	}
	char* p_a = GetCurPtr();
	a = *(int64_t*)(p_a);
	_cur_pos += sizeof(a);
	return *this;
}

BytesStream& BytesStream::operator<< (const float& a)
{
	Append(&a, sizeof(a));
	return *this;
}
BytesStream& BytesStream::operator>> (float& a)
{
	if (RemainSize() < sizeof(a))
	{
		throw ParserException();
	}
	char* p_a = GetCurPtr();
	a = *(float*)(p_a);
	_cur_pos += sizeof(a);
	return *this;
}

BytesStream& BytesStream::operator<< (const double& a)
{
	Append(&a, sizeof(a));
	return *this;
}
BytesStream& BytesStream::operator>> (double& a)
{
	if (RemainSize() < sizeof(a))
	{
		throw ParserException();
	}
	char* p_a = GetCurPtr();
	a = *(double*)(p_a);
	_cur_pos += sizeof(a);
	return *this;
}

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
