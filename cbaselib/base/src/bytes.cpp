#include "bytes.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <new>
#include <utility>

namespace OI { namespace Base {
char Bytes::_empty = 0;

Bytes::Bytes() : _mem(NULL), _cap(0), _head(0), _size(0) {}

Bytes::Bytes(const char* data, size_t len, uint32_t retain_len) : _mem(NULL), _cap(0), _head(0), _size(0)
{
	(void)retain_len;
	if (len == 0)
	{
		return;
	}
	assert(data);
	Grow(len);
	memcpy(Mutable(), data, len);
	_size = len;
}

Bytes::Bytes(const Bytes& other) : _mem(NULL), _cap(0), _head(0), _size(0)
{
	if (other._size > 0)
	{
		Grow(other._size);
		memcpy(Mutable(), other.Data(), other._size);
		_size = other._size;
	}
}

Bytes& Bytes::operator=(const Bytes& other)
{
	if (this != &other)
	{
		_head = 0;
		_size = 0;
		if (other._size > 0)
		{
			Grow(other._size);
			memcpy(Mutable(), other.Data(), other._size);
			_size = other._size;
		}
	}
	return *this;
}

Bytes::Bytes(Bytes&& other) : _mem(other._mem), _cap(other._cap), _head(other._head), _size(other._size)
{
	other._mem = NULL;
	other._cap = 0;
	other._head = 0;
	other._size = 0;
}

Bytes& Bytes::operator=(Bytes&& other)
{
	if (this != &other)
	{
		free(_mem);
		_mem = other._mem;
		_cap = other._cap;
		_head = other._head;
		_size = other._size;
		other._mem = NULL;
		other._cap = 0;
		other._head = 0;
		other._size = 0;
	}
	return *this;
}

Bytes::~Bytes()
{
	free(_mem);
}

void Bytes::Grow(size_t need)
{
	if (_head + need <= _cap)
	{
		return;
	}
	// 头部空出来的比有效数据多才挪，挪的字节不超过已经删掉的，均摊下来不会退化。
	if (need <= _cap && _head >= _size)
	{
		if (_size > 0)
		{
			memmove(_mem, _mem + _head, _size);
		}
		_head = 0;
		return;
	}
	size_t cap = _cap ? _cap * 2 : 64;
	while (cap < need)
	{
		cap *= 2;
	}
	char* mem = (char*)malloc(cap);
	if (!mem)
	{
		throw std::bad_alloc();
	}
	if (_size > 0)
	{
		memcpy(mem, _mem + _head, _size);
	}
	free(_mem);
	_mem = mem;
	_cap = cap;
	_head = 0;
}

char* Bytes::Mutable()
{
	return _mem ? _mem + _head : &_empty;
}

const char* Bytes::Data() const
{
	return _mem ? _mem + _head : &_empty;
}

char* Bytes::Begin()
{
	return Mutable();
}

char* Bytes::End()
{
	return Begin() + _size;
}

size_t Bytes::Reserve(size_t size)
{
	Grow(size);
	if (size < _size)
	{
		_size = size;
	}
	if (_size == 0)
	{
		_head = 0;
	}
	return Captity();
}

void Bytes::Commit(size_t n)
{
	assert(_size + n <= Captity());
	_size += n;
}

void Bytes::Swap(Bytes& other)
{
	std::swap(_mem, other._mem);
	std::swap(_cap, other._cap);
	std::swap(_head, other._head);
	std::swap(_size, other._size);
}

void Bytes::Clear()
{
	_size = 0;
	_head = 0;
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
	if (offset == 0)
	{
		_head += len;
		_size -= len;
		if (_size == 0)
		{
			_head = 0;
		}
		return len;
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
	if (offset == 0 && _head >= len)
	{
		_head -= len;
		_size += len;
		memcpy(Mutable(), data, len);
		return len;
	}
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
}}
