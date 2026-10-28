#pragma once
#include "oi_header.h"
#include <stddef.h>
#include <string>

namespace OI { namespace Base {
// 连续字节缓冲。
// 容量和有效长度分开：Begin() 在 [0, Captity()) 内可写，Size() 才是有效数据。
// 这样 read/aio 可以先 Reserve，再往 Begin() 里写，写完 Commit。
// 从头部删除只挪起点，不搬数据；扩容不清零，拷贝只拷有效数据。
class Bytes
{
	char* _mem;
	size_t _cap;
	size_t _head;
	size_t _size;
	static char _empty;

	void Grow(size_t need);
	char* Mutable();
public:
	Bytes();
	Bytes(const char* data, size_t len, uint32_t retain_len = 4096);
	Bytes(const Bytes& other);
	Bytes& operator=(const Bytes& other);
	Bytes(Bytes&& other);
	Bytes& operator=(Bytes&& other);
	virtual ~Bytes();

	void Erase(const void* begin, size_t len)
	{
		Erase((const char*)begin, len);
	}
	void Erase(const char* begin, size_t len);
	void Erase(int offset, size_t len)
	{
		if (offset >= 0)
		{
			Erase((size_t)offset, len);
		}
	}
	// 删除 [offset, offset+len)，超出末尾的部分会被截断
	size_t Erase(size_t offset, size_t len);

	// 插到 offset。offset == Size() 时等同 Append；offset 更大则断言
	size_t Insert(size_t offset, const void* data, size_t len);

	size_t Append(const void* data, size_t len);
	size_t Append(const Bytes& bytes);
	size_t AppendForward(const void* data, size_t len);

	// 覆盖 [offset, offset+len)。超出 Size() 会断言并截断
	size_t Replace(size_t offset, const void* data, size_t len);

	const char* Data() const;
	char* Begin();
	// 指向有效数据的末尾，这个位置本身不能读
	char* End();

	size_t Size() const { return _size; }
	size_t Captity() const { return _cap - _head; }

	// n 大于当前有效长度时只扩容，不改 Size()。
	// n 更小时把 Size() 收到 n，容量保留，避免写缓冲反复缩容。
	size_t Reserve(size_t size);
	// 直接往 End() 写了 n 字节后调用。n 不能超过 Captity() - Size()
	void Commit(size_t n);
	void Swap(Bytes& other);
	void Clear();
};
}}
