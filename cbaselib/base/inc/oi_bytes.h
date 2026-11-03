#pragma once
#include "oi_header.h"
#include <stddef.h>
#include <string>

namespace OI { namespace Base {
// 连续字节缓冲。
// 容量和有效长度分开：Begin() 在 [0, Capacity()) 内可写，Size() 才是有效数据。
// 这样 read/aio 可以先 Reserve，再往 Begin() 里写，写完 Commit。
// 从头部删除只挪起点，不搬数据；扩容不清零，拷贝只拷有效数据。
class Bytes
{
	char* m_pMemory;
	size_t m_iCapacity;
	size_t m_iHead;
	size_t m_iSize;
	static char m_iEmptyCharacter;

	void Grow(size_t iNeedParam);
	char* Mutable();
	bool Overlaps(const void* pDataParam) const;
public:
	Bytes();
	Bytes(const char* pDataParam, size_t iLengthParam);
	Bytes(const Bytes& stOtherParam);
	Bytes& operator=(const Bytes& stOtherParam);
	Bytes(Bytes&& stOtherParam);
	Bytes& operator=(Bytes&& stOtherParam);
	virtual ~Bytes();

	void Erase(const void* pBeginParam, size_t iLengthParam)
	{
		Erase((const char*)pBeginParam, iLengthParam);
	}
	void Erase(const char* pBeginParam, size_t iLengthParam);
	void Erase(int iOffsetParam, size_t iLengthParam)
	{
		if (iOffsetParam >= 0)
		{
			Erase((size_t)iOffsetParam, iLengthParam);
		}
	}
	// 删除 [offset, offset+len)，超出末尾的部分会被截断
	size_t Erase(size_t iOffsetParam, size_t iLengthParam);

	// 插到 offset。offset == Size() 时等同 Append；offset 更大则断言
	size_t Insert(size_t iOffsetParam, const void* pDataParam, size_t iLengthParam);

	size_t Append(const void* pDataParam, size_t iLengthParam);
	size_t Append(const Bytes& stBytesParam);
	size_t AppendForward(const void* pDataParam, size_t iLengthParam);

	// 覆盖 [offset, offset+len)。超出 Size() 会断言并截断
	size_t Replace(size_t iOffsetParam, const void* pDataParam, size_t iLengthParam);

	const char* Data() const;
	char* Begin();
	// 指向有效数据的末尾，这个位置本身不能读
	char* End();

	size_t Size() const { return m_iSize; }
	size_t Capacity() const { return m_iCapacity - m_iHead; }

	// n 大于当前有效长度时只扩容，不改 Size()。
	// n 更小时把 Size() 收到 n，容量保留，避免写缓冲反复缩容。
	size_t Reserve(size_t iSizeParam);
	// 直接往 End() 写了 n 字节后调用。n 超过 Capacity() - Size() 抛 std::out_of_range
	void Commit(size_t iNumberParam);
	void Swap(Bytes& stOtherParam);
	void Clear();
};
}}
