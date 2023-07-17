#include "bytes.h"
#include <stdlib.h>
#include <assert.h>
#include <stddef.h>
#include <string.h>

//TODO 检查一下memmove有没有检查内存区域重叠而选择是否使用缓冲区,如果不是,建议自己手写一个memmove(),减少内存拷贝

Bytes::Bytes()
{
	_content = Bytes::GetDefaultBytes().RefContent();
}

Bytes::Bytes(const char* data, size_t len, uint32_t retain_len)
{
	_content = CreateContent(len, retain_len);
	memcpy(_content->_data, data, len);
}

Bytes::Bytes(const Bytes& bytes)
{
	_content = const_cast<Bytes&>(bytes).RefContent();
}

Bytes& Bytes::operator= (const Bytes& bytes)
{
	_content = const_cast<Bytes&>(bytes).RefContent();
	return *this;
}

#if __cplusplus >= 201103L 
Bytes::Bytes(const Bytes&& that)
{
	//暂时右值引用一样处理吧.
	_content = const_cast<Bytes&>(that).RefContent();
}
#endif


Bytes::~Bytes()
{
	DestroyContent();
}

void Bytes::Erase(const char* begin, size_t len)
{
	if (begin < Begin())
	{
		return;
	}
	if (len <= 0)
	{
		assert(len == 0);
		return;
	}
	size_t offset = begin - Begin();
	Erase(offset, len);
}

size_t Bytes::Erase(size_t offset, size_t len)
{
	Unique();
	if (offset > Size())
	{
		return 0;
	}
	if (offset + len > Size())
	{
		len = Size() - offset;
	}
	//_data --> [offset --> offset+len] -->  _data_len
	memmove(&_content->_data[offset], &_content->_data[offset + len], Size() - offset - len);
	Resize(Size() - len);
	return len;
}

size_t Bytes::Insert(size_t offset, const void* data, size_t len)
{
	if (offset >= Size())
	{
		//越界直接append
		//TODO LOG_WARNING
		assert(offset == Size());
		return Append(data, len);
	}
	Unique();
	//_data -->offset --->offset+len --> _data_len
	size_t data_len = Size();
	Resize(Size() + len);
	//挪offset-->data_len
	memmove(&_content->_data[offset+len], &_content->_data[offset], data_len - offset);
	//拷贝新的data
	memmove(&_content->_data[offset], data, len);
	return len;
}

size_t Bytes::Append(const void* data, size_t len)
{
	Unique();
	size_t data_len = Size();
	Resize(Size() + len);
	memmove(&_content->_data[data_len], data, len);
	return len;
}

size_t Bytes::Append(const Bytes& bytes)
{
	return Append(bytes._content->_data, bytes.Size());
}


size_t	Bytes::AppendForward(const void* data, size_t len)
{
	return Insert(0, data, len);
}


size_t Bytes::Replace(size_t offset, const void* data, size_t len)
{
	Unique();
	if (Size() < offset + len)
	{
		//TODO LOGWARNING
		assert(false);
		len = Size() - offset;
	}
	memmove(&_content->_data[offset], data, len);
	return len;
}

char* Bytes::Begin()
{
	Unique();
	return _content->_data;
}

char* Bytes::End()
{
	Unique();
	return _content->_data + _content->_data_len;
}

size_t Bytes::Size() const
{
	//Unique();
	return _content->_data_len;
}

size_t	Bytes::Captity() const
{
	//Unique();
	return _content->_captity;
}

size_t Bytes::Reserve(size_t size)
{
	Unique();
	size_t data_len = Size();
	Resize(size);
	if (data_len < Size())
	{
		//扩容的情况,不扩容_data_len
		_content->_data_len = data_len;
		size_t retain_len = Captity() - Size();
		//避免扩容后,频繁缩容,白扩
		if (_content->_retain_len < retain_len)
		{
			_content->_retain_len = retain_len;
		}
	}
	return GetContent()->_captity;
}

void Bytes::Clear()
{
#if 0
	DestroyContent();
	_content = Bytes::GetDefaultBytes().RefContent();
#else
	Reserve(0);
#endif
}


void Bytes::Unique()
{
	//TODO 考虑一下多线程此处需要原子操作否
	//场景1: 线程1线程2同时unique会不会有问题
	//不会,因为unique先克隆再减引用,而引用减少为原子操作,所以最多两个同时unique,同时dupcontent,把原有内存干掉
	//场景2: 场景1的情况 线程3增加引用数. 不现实,因为线程3不会与线程2和线程1公用一个Bytes,因为这样本来就会有问题,原子操作保护同一个content,但是不保护Bytes自身
	
	if (_content->_ref > 1)	//这里不需要加锁,可以想想为什么
	{
		BytesContent* tmp = DupContent(_content); //必须先克隆
		DestroyContent();
		_content = tmp;
	}
}

void Bytes::Resize(size_t cap)
{
	assert(_content->_ref == 1);

	_content->_data_len = cap;
	/*add_cap*/		/*dec cap*/
	if (cap > _content->_captity || cap < _content->_captity - _content->_retain_len)
	{
		//容量小于保留容量时,修正为保留容量,避免频繁扩容
		cap = cap < _content->_retain_len ? _content->_retain_len : cap;

		_content = (BytesContent*)realloc(_content, sizeof(BytesContent) + cap);
		_content->_captity = cap;
	}
}


const Bytes::BytesContent* Bytes::GetContent() const
{
	return _content;
}

Bytes::BytesContent* Bytes::RefContent()
{
#ifdef _SINGLE_THREAD_
	++_content->_ref;
#else
	__sync_fetch_and_add(&_content->_ref, 1);
#endif
	return _content;
}

void Bytes::DestroyContent()
{
#ifdef _SINGLE_THREAD_
	int old = _content->_ref--;
#else
	int old = __sync_fetch_and_sub(&_content->_ref, 1);
#endif
	if (old <= 1)
	{
		assert(old == 1);
		DestroyContent(_content);
	}
}

Bytes::BytesContent* Bytes::DupContent(const BytesContent* p_content)
{
	BytesContent* pContent = (BytesContent*)malloc(sizeof(BytesContent) + p_content->_data_len);
	pContent->_data_len = p_content->_data_len;
	pContent->_captity = pContent->_data_len;
	pContent->_ref = 1;
	pContent->_retain_len = p_content->_retain_len;
	memcpy(pContent->_data, p_content->_data, pContent->_data_len);
	return pContent;
}

Bytes::BytesContent* Bytes::CreateContent(size_t data_len, uint32_t retain_len)
{
	BytesContent* pContent = (BytesContent*)malloc(sizeof(BytesContent) + data_len);
	if (pContent)
	{
		pContent->_captity = data_len;
		pContent->_retain_len = retain_len;
		pContent->_data_len = data_len;
		pContent->_ref = 1;
	}
	return pContent;
}

void Bytes::DestroyContent(BytesContent* p_content)
{
	SAFE_FREE(p_content);
}

Bytes& Bytes::GetDefaultBytes()
{
	static Bytes s_default_bytes(0, 0, 0);
	return s_default_bytes;
}
