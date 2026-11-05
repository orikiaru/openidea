#include "oi_bytes.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <new>
#include <stdexcept>
#include <utility>

namespace OI
{
namespace Base
{
char Bytes::m_iEmptyCharacter = 0;

Bytes::Bytes()
    : m_pMemory(NULL), m_iCapacity(0), m_iHead(0), m_iSize(0)
{
}

Bytes::Bytes(const char* pData, size_t iLength)
    : m_pMemory(NULL), m_iCapacity(0), m_iHead(0), m_iSize(0)
{
	if (iLength == 0)
	{
		return;
	}
	assert(pData);
	Grow(iLength);
	memcpy(Mutable(), pData, iLength);
	m_iSize = iLength;
}

Bytes::Bytes(const Bytes& stOther)
    : m_pMemory(NULL), m_iCapacity(0), m_iHead(0), m_iSize(0)
{
	if (stOther.m_iSize > 0)
	{
		Grow(stOther.m_iSize);
		memcpy(Mutable(), stOther.Data(), stOther.m_iSize);
		m_iSize = stOther.m_iSize;
	}
}

Bytes& Bytes::operator=(const Bytes& stOther)
{
	if (this != &stOther)
	{
		m_iHead = 0;
		m_iSize = 0;
		if (stOther.m_iSize > 0)
		{
			Grow(stOther.m_iSize);
			memcpy(Mutable(), stOther.Data(), stOther.m_iSize);
			m_iSize = stOther.m_iSize;
		}
	}
	return *this;
}

Bytes::Bytes(Bytes&& stOther)
    : m_pMemory(stOther.m_pMemory), m_iCapacity(stOther.m_iCapacity), m_iHead(stOther.m_iHead), m_iSize(stOther.m_iSize)
{
	stOther.m_pMemory = NULL;
	stOther.m_iCapacity = 0;
	stOther.m_iHead = 0;
	stOther.m_iSize = 0;
}

Bytes& Bytes::operator=(Bytes&& stOther)
{
	if (this != &stOther)
	{
		free(m_pMemory);
		m_pMemory = stOther.m_pMemory;
		m_iCapacity = stOther.m_iCapacity;
		m_iHead = stOther.m_iHead;
		m_iSize = stOther.m_iSize;
		stOther.m_pMemory = NULL;
		stOther.m_iCapacity = 0;
		stOther.m_iHead = 0;
		stOther.m_iSize = 0;
	}
	return *this;
}

Bytes::~Bytes()
{
	free(m_pMemory);
}

void Bytes::Grow(size_t iNeed)
{
	if (m_iHead + iNeed <= m_iCapacity)
	{
		return;
	}
	// 头部空出来的比有效数据多才挪，挪的字节不超过已经删掉的，均摊下来不会退化。
	if (iNeed <= m_iCapacity && m_iHead >= m_iSize)
	{
		if (m_iSize > 0)
		{
			memmove(m_pMemory, m_pMemory + m_iHead, m_iSize);
		}
		m_iHead = 0;
		return;
	}
	size_t iCapacity = m_iCapacity ? m_iCapacity : 32;
	do
	{
		if (iCapacity > SIZE_MAX / 2)
		{
			throw std::bad_alloc();
		}
		iCapacity *= 2;
	} while (iCapacity < iNeed);
	char* pMem = (char*)malloc(iCapacity);
	if (!pMem)
	{
		throw std::bad_alloc();
	}
	if (m_iSize > 0)
	{
		memcpy(pMem, m_pMemory + m_iHead, m_iSize);
	}
	free(m_pMemory);
	m_pMemory = pMem;
	m_iCapacity = iCapacity;
	m_iHead = 0;
}

char* Bytes::Mutable()
{
	return m_pMemory ? m_pMemory + m_iHead : &m_iEmptyCharacter;
}

const char* Bytes::Data() const
{
	return m_pMemory ? m_pMemory + m_iHead : &m_iEmptyCharacter;
}

char* Bytes::Begin()
{
	return Mutable();
}

char* Bytes::End()
{
	return Begin() + m_iSize;
}

size_t Bytes::Reserve(size_t iSize)
{
	Grow(iSize);
	if (iSize < m_iSize)
	{
		m_iSize = iSize;
	}
	if (m_iSize == 0)
	{
		m_iHead = 0;
	}
	return Capacity();
}

void Bytes::Commit(size_t iNumber)
{
	if (iNumber > Capacity() - m_iSize)
	{
		throw std::out_of_range("Bytes::Commit past capacity");
	}
	m_iSize += iNumber;
}

bool Bytes::Overlaps(const void* pData) const
{
	const char* pPointer = (const char*)pData;
	return m_pMemory && pPointer >= m_pMemory && pPointer < m_pMemory + m_iCapacity;
}

void Bytes::Swap(Bytes& stOther)
{
	std::swap(m_pMemory, stOther.m_pMemory);
	std::swap(m_iCapacity, stOther.m_iCapacity);
	std::swap(m_iHead, stOther.m_iHead);
	std::swap(m_iSize, stOther.m_iSize);
}

void Bytes::Clear()
{
	m_iSize = 0;
	m_iHead = 0;
}

size_t Bytes::Append(const void* pData, size_t iLength)
{
	if (iLength == 0)
	{
		return 0;
	}
	assert(pData);
	if (iLength > SIZE_MAX - m_iSize)
	{
		throw std::bad_alloc();
	}
	// 源在自己的缓冲里，扩容会把它释放掉。先拷出来。
	if (Overlaps(pData))
	{
		Bytes stCopy((const char*)pData, iLength);
		return Append(stCopy.Data(), iLength);
	}
	Grow(m_iSize + iLength);
	memcpy(Mutable() + m_iSize, pData, iLength);
	m_iSize += iLength;
	return iLength;
}

size_t Bytes::Append(const Bytes& stBytes)
{
	return Append(stBytes.Data(), stBytes.Size());
}

size_t Bytes::AppendForward(const void* pData, size_t iLength)
{
	return Insert(0, pData, iLength);
}

size_t Bytes::Erase(size_t iOffset, size_t iLength)
{
	if (iOffset > m_iSize)
	{
		return 0;
	}
	if (iLength > m_iSize - iOffset)
	{
		iLength = m_iSize - iOffset;
	}
	if (iLength == 0)
	{
		return 0;
	}
	if (iOffset == 0)
	{
		m_iHead += iLength;
		m_iSize -= iLength;
		if (m_iSize == 0)
		{
			m_iHead = 0;
		}
		return iLength;
	}
	memmove(Mutable() + iOffset, Mutable() + iOffset + iLength, m_iSize - iOffset - iLength);
	m_iSize -= iLength;
	return iLength;
}

void Bytes::Erase(const char* pBegin, size_t iLength)
{
	const char* pBase = Data();
	if (pBegin < pBase || pBegin > pBase + m_iSize)
	{
		return;
	}
	Erase((size_t)(pBegin - pBase), iLength);
}

size_t Bytes::Insert(size_t iOffset, const void* pData, size_t iLength)
{
	if (iOffset >= m_iSize)
	{
		assert(iOffset == m_iSize);
		return Append(pData, iLength);
	}
	if (iLength == 0)
	{
		return 0;
	}
	assert(pData);
	if (iLength > SIZE_MAX - m_iSize)
	{
		throw std::bad_alloc();
	}
	if (Overlaps(pData))
	{
		Bytes stCopy((const char*)pData, iLength);
		return Insert(iOffset, stCopy.Data(), iLength);
	}
	if (iOffset == 0 && m_iHead >= iLength)
	{
		m_iHead -= iLength;
		m_iSize += iLength;
		memcpy(Mutable(), pData, iLength);
		return iLength;
	}
	Grow(m_iSize + iLength);
	memmove(Mutable() + iOffset + iLength, Mutable() + iOffset, m_iSize - iOffset);
	memcpy(Mutable() + iOffset, pData, iLength);
	m_iSize += iLength;
	return iLength;
}

size_t Bytes::Replace(size_t iOffset, const void* pData, size_t iLength)
{
	if (iOffset > m_iSize || iLength > m_iSize - iOffset)
	{
		assert(false);
		if (iOffset >= m_iSize)
		{
			return 0;
		}
		iLength = m_iSize - iOffset;
	}
	if (iLength == 0)
	{
		return 0;
	}
	assert(pData);
	memcpy(Mutable() + iOffset, pData, iLength);
	return iLength;
}
}
}
