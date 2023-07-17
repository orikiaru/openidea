#pragma once
#include "header.h"
#include "stddef.h"
enum
{
	BytesContent_DEFAULT_RETAIN_LEN = 4096,	//允许空闲的内存最大字节数
};

typedef volatile uint32_t v_uint32_t;

//Bytes本身需要锁保护
//而内部的BytesContent操作已经按照原子操作封装
//保证复数个引用的数据,只要有修改就唯一化
class Bytes
{
protected:
	class BytesContent
	{
		friend class Bytes;	//不允许此类引用
		size_t		_data_len;
		size_t		_captity;
		uint32_t	_retain_len;	//允许空闲的内存最大字节数,避免删除内存后频繁重复分配
#ifdef _SINGLE_THREAD_
		uint32_t	_ref;
#else
		v_uint32_t	_ref;
#endif
		char		_data[];
	};
	BytesContent* _content;
public:
	Bytes();
	Bytes(const char* data, size_t len, uint32_t retain_len = BytesContent_DEFAULT_RETAIN_LEN);
	Bytes(const Bytes& bytes);
	Bytes& operator= (const Bytes&);
#if __cplusplus >= 201103L 
	Bytes(const Bytes&& that);
#endif
	virtual	~Bytes();

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
	//将范围在_data[offset]-->_data[offset+len]区间数据删除,容错到end
	size_t	Erase(size_t offset, size_t len);

	//将数据插入到_data[offset]->_data[offset+len]区间,容错到end,offset一旦超过end将直接异常
	size_t	Insert(size_t offset, const void* data, size_t len);

	//向尾部追加
	size_t	Append(const void* data, size_t len);
	//向尾部追加
	size_t	Append(const Bytes& bytes);
	//向头部追加,特殊的Insert操作
	size_t	AppendForward(const void* data, size_t len);

	//替换范围在[_data + offset, _data + offset + len)的数据为[data, data+len)的数据,一旦offset+len>Size()将直接异常
	size_t	Replace(size_t offset, const void* data, size_t len);

	//data[0]
	char*	Begin();
	//data[Size()] 注:这块内存一定无效,不要访问.
	char*	End();

	//有效长度
	size_t	Size() const;
	//实际容量
	size_t	Captity() const;

	//扩容,目前直接调用Resize,支持缩容,缩容后Size会被直接修改,扩容则只扩容量,避免内存重复分配
	//Reserve扩容后,将会使retain_len尝试增大到 Captity()-Size();
	size_t  Reserve(size_t size);
	//清除当前缓存
	void	Clear();


private:
	void Unique();
	void Resize(size_t data_len);	//Resize前必须Unique()
	const BytesContent* GetContent() const;
	BytesContent* RefContent();
	void DestroyContent();
private:
	static BytesContent* DupContent(const BytesContent* p_content) ;
	static BytesContent* CreateContent(size_t data_len, uint32_t retain_len);
	static void DestroyContent(BytesContent* p_content);

	static Bytes& GetDefaultBytes();
};
