#include "bytes.h"
#include <assert.h>
#include <string.h>

char Bytes::_empty = 0;

Bytes::Bytes() : _size(0) {}

Bytes::Bytes(const char* data, size_t len, uint32_t retain_len) : _size(0)
{
	(void)retain_len;
	if (len == 0)
	{
		return;
	}
	assert(data);
	_buf.resize(len);
	memcpy(Mutable(), data, len);
	_size = len;
}

Bytes::Bytes(const Bytes& other) : _buf(other._buf), _size(other._size) {}

Bytes& Bytes::operator=(const Bytes& other)
{
	if (this != &other)
	{
		_buf = other._buf;
		_size = other._size;
	}
	return *this;
}

Bytes::Bytes(Bytes&& other) : _buf(std::move(other._buf)), _size(other._size)
{
	other._size = 0;
}

Bytes& Bytes::operator=(Bytes&& other)
{
	if (this != &other)
	{
		_buf = std::move(other._buf);
		_size = other._size;
		other._size = 0;
	}
	return *this;
}

Bytes::~Bytes() {}

void Bytes::Grow(size_t need)
{
	if (need <= _buf.size())
	{
		return;
	}
	size_t cap = _buf.empty() ? 64 : _buf.size() * 2;
	while (cap < need)
	{
		cap *= 2;
	}
	_buf.resize(cap);
}

char* Bytes::Mutable()
{
	return _buf.empty() ? &_empty : &_buf[0];
}

const char* Bytes::Data() const
{
	return _buf.empty() ? &_empty : _buf.data();
}

char* Bytes::Begin()
{
	return const_cast<char*>(Data());
}

char* Bytes::End()
{
	return Begin() + _size;
}

size_t Bytes::Reserve(size_t size)
{
	if (size > _buf.size())
	{
		_buf.resize(size);
	}
	if (size < _size)
	{
		_size = size;
	}
	return _buf.size();
}

void Bytes::Clear()
{
	_size = 0;
}

size_t Bytes::Append(const void* data, size_t len)
{
	if (len == 0)
	{
		return 0;
	}
	assert(data);
	Grow(_size + len);
	memcpy(Mutable() + _size, data, len);
	_size += len;
	return len;
}

size_t Bytes::Append(const Bytes& bytes)
{
	return Append(bytes.Data(), bytes.Size());
}

size_t Bytes::AppendForward(const void* data, size_t len)
{
	return Insert(0, data, len);
}

size_t Bytes::Erase(size_t offset, size_t len)
{
	if (offset > _size)
	{
		return 0;
	}
	if (len > _size - offset)
	{
		len = _size - offset;
	}
	if (len == 0)
	{
		return 0;
	}
	memmove(Mutable() + offset, Mutable() + offset + len, _size - offset - len);
	_size -= len;
	return len;
}

void Bytes::Erase(const char* begin, size_t len)
{
	const char* base = Data();
	if (begin < base || begin > base + _size)
	{
		return;
	}
	Erase((size_t)(begin - base), len);
}

size_t Bytes::Insert(size_t offset, const void* data, size_t len)
{
	if (offset >= _size)
	{
		assert(offset == _size);
		return Append(data, len);
	}
	if (len == 0)
	{
		return 0;
	}
	assert(data);
	Grow(_size + len);
	memmove(Mutable() + offset + len, Mutable() + offset, _size - offset);
	memcpy(Mutable() + offset, data, len);
	_size += len;
	return len;
}

size_t Bytes::Replace(size_t offset, const void* data, size_t len)
{
	if (offset > _size || len > _size - offset)
	{
		assert(false);
		if (offset >= _size)
		{
			return 0;
		}
		len = _size - offset;
	}
	if (len == 0)
	{
		return 0;
	}
	assert(data);
	memcpy(Mutable() + offset, data, len);
	return len;
}
