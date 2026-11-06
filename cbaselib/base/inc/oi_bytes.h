#pragma once
#include "oi_header.h"
#include <stddef.h>
#include <string>

namespace OI
{
namespace Base
{
// 连续字节缓冲。
// 容量和有效长度分开：Begin() 在 [0, Capacity()) 内可写，Size() 才是有效数据。
// 这样 read/aio 可以先 Reserve，再往 Begin() 里写，写完 Commit。
// 从头部删除只挪起点，不搬数据；扩容不清零，拷贝只拷有效数据。
class Bytes
{
	// 一整块分配。空缓冲不使用它。
	char* m_pMemory;
	// 分配长度。
	size_t m_iCapacity;
	// 有效数据在块里的起点。从头部删除只增加它，不搬数据。
	size_t m_iHead;
	// 有效字节数。
	size_t m_iSize;
	// 空缓冲的 Data() 指向它，避免返回空指针。
	static char m_iEmptyCharacter;

	// 容量不够时换一块更大的。
	void Grow(size_t iNeed);
	// 可写起点。
	char* Mutable();
	// 源区间是否落在本缓冲里。插入时用来避免覆盖自己。
	bool Overlaps(const void* pData) const;

  public:
	Bytes();
	Bytes(const char* pData, size_t iLength);
	Bytes(const Bytes& stOther);
	Bytes& operator=(const Bytes& stOther);
	Bytes(Bytes&& stOther);
	Bytes& operator=(Bytes&& stOther);
	virtual ~Bytes();

	void Erase(const void* pBegin, size_t iLength)
	{
		Erase((const char*)pBegin, iLength);
	}
	void Erase(const char* pBegin, size_t iLength);
	void Erase(int iOffset, size_t iLength)
	{
		if (iOffset >= 0)
		{
			Erase((size_t)iOffset, iLength);
		}
	}
	// 删除 [offset, offset+len)，超出末尾的部分会被截断
	size_t Erase(size_t iOffset, size_t iLength);

	// 插到 offset。offset == Size() 时等同 Append；offset 更大则断言
	size_t Insert(size_t iOffset, const void* pData, size_t iLength);

	// 追加到末尾。返回写入的字节数。
	size_t Append(const void* pData, size_t iLength);
	size_t Append(const Bytes& stBytes);
	// 插到开头。
	size_t AppendForward(const void* pData, size_t iLength);

	// 覆盖 [offset, offset+len)。超出 Size() 会断言并截断
	size_t Replace(size_t iOffset, const void* pData, size_t iLength);

	// 有效数据起点，只读。空缓冲不返回空指针。
	const char* Data() const;
	// 可写起点，范围到 Capacity()。
	char* Begin();
	// 指向有效数据的末尾，这个位置本身不能读
	char* End();

	size_t Size() const
	{
		return m_iSize;
	}
	size_t Capacity() const
	{
		return m_iCapacity - m_iHead;
	}

	// n 大于当前有效长度时只扩容，不改 Size()。
	// n 更小时把 Size() 收到 n，容量保留，避免写缓冲反复缩容。
	size_t Reserve(size_t iSize);
	// 直接往 End() 写了 n 字节后调用。n 超过 Capacity() - Size() 抛 std::out_of_range
	void Commit(size_t iNumber);
	// 交换两块缓冲的指针和长度。
	void Swap(Bytes& stOther);
	// 有效数据清掉，容量保留。
	void Clear();
};
}
}
