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

Bytes::Bytes(const char* pDataParam, size_t iLengthParam)
    : m_pMemory(NULL), m_iCapacity(0), m_iHead(0), m_iSize(0)
{
	if (iLengthParam == 0)
	{
		return;
	}
	assert(pDataParam);
	Grow(iLengthParam);
	memcpy(Mutable(), pDataParam, iLengthParam);
	m_iSize = iLengthParam;
}

Bytes::Bytes(const Bytes& stOtherParam)
    : m_pMemory(NULL), m_iCapacity(0), m_iHead(0), m_iSize(0)
{
	if (stOtherParam.m_iSize > 0)
	{
		Grow(stOtherParam.m_iSize);
		memcpy(Mutable(), stOtherParam.Data(), stOtherParam.m_iSize);
		m_iSize = stOtherParam.m_iSize;
	}
}

Bytes& Bytes::operator=(const Bytes& stOtherParam)
{
	if (this != &stOtherParam)
	{
		m_iHead = 0;
		m_iSize = 0;
		if (stOtherParam.m_iSize > 0)
		{
			Grow(stOtherParam.m_iSize);
			memcpy(Mutable(), stOtherParam.Data(), stOtherParam.m_iSize);
			m_iSize = stOtherParam.m_iSize;
		}
	}
	return *this;
}

Bytes::Bytes(Bytes&& stOtherParam)
    : m_pMemory(stOtherParam.m_pMemory), m_iCapacity(stOtherParam.m_iCapacity), m_iHead(stOtherParam.m_iHead), m_iSize(stOtherParam.m_iSize)
{
	stOtherParam.m_pMemory = NULL;
	stOtherParam.m_iCapacity = 0;
	stOtherParam.m_iHead = 0;
	stOtherParam.m_iSize = 0;
}

Bytes& Bytes::operator=(Bytes&& stOtherParam)
{
	if (this != &stOtherParam)
	{
		free(m_pMemory);
		m_pMemory = stOtherParam.m_pMemory;
		m_iCapacity = stOtherParam.m_iCapacity;
		m_iHead = stOtherParam.m_iHead;
		m_iSize = stOtherParam.m_iSize;
		stOtherParam.m_pMemory = NULL;
		stOtherParam.m_iCapacity = 0;
		stOtherParam.m_iHead = 0;
		stOtherParam.m_iSize = 0;
	}
	return *this;
}

Bytes::~Bytes()
{
	free(m_pMemory);
}

void Bytes::Grow(size_t iNeedParam)
{
	if (m_iHead + iNeedParam <= m_iCapacity)
	{
		return;
	}
	// 头部空出来的比有效数据多才挪，挪的字节不超过已经删掉的，均摊下来不会退化。
	if (iNeedParam <= m_iCapacity && m_iHead >= m_iSize)
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
	} while (iCapacity < iNeedParam);
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

size_t Bytes::Reserve(size_t iSizeParam)
{
	Grow(iSizeParam);
	if (iSizeParam < m_iSize)
	{
		m_iSize = iSizeParam;
	}
	if (m_iSize == 0)
	{
		m_iHead = 0;
	}
	return Capacity();
}

void Bytes::Commit(size_t iNumberParam)
{
	if (iNumberParam > Capacity() - m_iSize)
	{
		throw std::out_of_range("Bytes::Commit past capacity");
	}
	m_iSize += iNumberParam;
}

bool Bytes::Overlaps(const void* pDataParam) const
{
	const char* pPointer = (const char*)pDataParam;
	return m_pMemory && pPointer >= m_pMemory && pPointer < m_pMemory + m_iCapacity;
}

void Bytes::Swap(Bytes& stOtherParam)
{
	std::swap(m_pMemory, stOtherParam.m_pMemory);
	std::swap(m_iCapacity, stOtherParam.m_iCapacity);
	std::swap(m_iHead, stOtherParam.m_iHead);
	std::swap(m_iSize, stOtherParam.m_iSize);
}

void Bytes::Clear()
{
	m_iSize = 0;
	m_iHead = 0;
}

size_t Bytes::Append(const void* pDataParam, size_t iLengthParam)
{
	if (iLengthParam == 0)
	{
		return 0;
	}
	assert(pDataParam);
	if (iLengthParam > SIZE_MAX - m_iSize)
	{
		throw std::bad_alloc();
	}
	// 源在自己的缓冲里，扩容会把它释放掉。先拷出来。
	if (Overlaps(pDataParam))
	{
		Bytes stCopy((const char*)pDataParam, iLengthParam);
		return Append(stCopy.Data(), iLengthParam);
	}
	Grow(m_iSize + iLengthParam);
	memcpy(Mutable() + m_iSize, pDataParam, iLengthParam);
	m_iSize += iLengthParam;
	return iLengthParam;
}

size_t Bytes::Append(const Bytes& stBytesParam)
{
	return Append(stBytesParam.Data(), stBytesParam.Size());
}

size_t Bytes::AppendForward(const void* pDataParam, size_t iLengthParam)
{
	return Insert(0, pDataParam, iLengthParam);
}

size_t Bytes::Erase(size_t iOffsetParam, size_t iLengthParam)
{
	if (iOffsetParam > m_iSize)
	{
		return 0;
	}
	if (iLengthParam > m_iSize - iOffsetParam)
	{
		iLengthParam = m_iSize - iOffsetParam;
	}
	if (iLengthParam == 0)
	{
		return 0;
	}
	if (iOffsetParam == 0)
	{
		m_iHead += iLengthParam;
		m_iSize -= iLengthParam;
		if (m_iSize == 0)
		{
			m_iHead = 0;
		}
		return iLengthParam;
	}
	memmove(Mutable() + iOffsetParam, Mutable() + iOffsetParam + iLengthParam, m_iSize - iOffsetParam - iLengthParam);
	m_iSize -= iLengthParam;
	return iLengthParam;
}

void Bytes::Erase(const char* pBeginParam, size_t iLengthParam)
{
	const char* pBase = Data();
	if (pBeginParam < pBase || pBeginParam > pBase + m_iSize)
	{
		return;
	}
	Erase((size_t)(pBeginParam - pBase), iLengthParam);
}

size_t Bytes::Insert(size_t iOffsetParam, const void* pDataParam, size_t iLengthParam)
{
	if (iOffsetParam >= m_iSize)
	{
		assert(iOffsetParam == m_iSize);
		return Append(pDataParam, iLengthParam);
	}
	if (iLengthParam == 0)
	{
		return 0;
	}
	assert(pDataParam);
	if (iLengthParam > SIZE_MAX - m_iSize)
	{
		throw std::bad_alloc();
	}
	if (Overlaps(pDataParam))
	{
		Bytes stCopy((const char*)pDataParam, iLengthParam);
		return Insert(iOffsetParam, stCopy.Data(), iLengthParam);
	}
	if (iOffsetParam == 0 && m_iHead >= iLengthParam)
	{
		m_iHead -= iLengthParam;
		m_iSize += iLengthParam;
		memcpy(Mutable(), pDataParam, iLengthParam);
		return iLengthParam;
	}
	Grow(m_iSize + iLengthParam);
	memmove(Mutable() + iOffsetParam + iLengthParam, Mutable() + iOffsetParam, m_iSize - iOffsetParam);
	memcpy(Mutable() + iOffsetParam, pDataParam, iLengthParam);
	m_iSize += iLengthParam;
	return iLengthParam;
}

size_t Bytes::Replace(size_t iOffsetParam, const void* pDataParam, size_t iLengthParam)
{
	if (iOffsetParam > m_iSize || iLengthParam > m_iSize - iOffsetParam)
	{
		assert(false);
		if (iOffsetParam >= m_iSize)
		{
			return 0;
		}
		iLengthParam = m_iSize - iOffsetParam;
	}
	if (iLengthParam == 0)
	{
		return 0;
	}
	assert(pDataParam);
	memcpy(Mutable() + iOffsetParam, pDataParam, iLengthParam);
	return iLengthParam;
}
}
}
