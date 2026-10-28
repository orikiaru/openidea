#pragma once
#include "bytes.h"
#include <vector>
#include <list>
#include <map>
#include <string>
#include <string.h>
#include <type_traits>

namespace OI { namespace Rpc { class Serialize; } }
namespace OI { namespace Base {
struct ParserException
{
	const char* what() const throw()
	{
		return "ParserException";
	}
};

// PreparePbHeader 的结果。BAD 表示 seq/size 最高位为 1（与原先有符号 int 头不兼容）。
enum PbHeaderState
{
	PB_HEADER_OK = 0,
	PB_HEADER_NEED_MORE,
	PB_HEADER_BAD,
};

template<typename T>
struct PbFixedTrait
{
	static const bool value = std::is_arithmetic<T>::value || std::is_enum<T>::value;
};

class BytesStream :public Bytes
{
	size_t 	_cur_pos;	//反序列化时使用
	size_t  _snapshot_pos;	//快照位置
public:
	BytesStream() :_cur_pos(0), _snapshot_pos(0) {}
	BytesStream(const Bytes& bytes) :Bytes(bytes), _cur_pos(0), _snapshot_pos(0) {}
	BytesStream(const char* data, size_t data_len) :Bytes(data, data_len), _cur_pos(0), _snapshot_pos(0) {}
	void Clear()
	{
		Bytes::Clear();
		_cur_pos = 0;
		_snapshot_pos = 0;
	}
	Bytes& GetMutableBuff() { return *this; }
	const Bytes& GetUnMutableBuff()const { return *this; }
	size_t	GetCurPos()const { return _cur_pos; }
	void 	AddCurPos(size_t offset) { _cur_pos += offset; }
	char*	GetCurPtr() { return &(Begin()[_cur_pos]); }
	size_t	RemainSize()const { return Size() - _cur_pos; }

	void	StartTransaction() { _snapshot_pos = _cur_pos; }
	void  	RollBack() { _cur_pos = _snapshot_pos; }

	template<typename T>
	typename std::enable_if<PbFixedTrait<T>::value, BytesStream&>::type
	operator<< (const T& value)
	{
		Append(&value, sizeof(T));
		return *this;
	}
	template<typename T>
	typename std::enable_if<PbFixedTrait<T>::value, BytesStream&>::type
	operator>> (T& value)
	{
		if (RemainSize() < sizeof(T))
		{
			throw ParserException();
		}
		memcpy(&value, GetCurPtr(), sizeof(T));
		_cur_pos += sizeof(T);
		return *this;
	}

	BytesStream& operator<< (const std::string& a);
	BytesStream& operator<< (const Bytes& a);
	BytesStream& operator<< (const BytesStream& a);	//全量写入，内容本身已经是序列化结果

	BytesStream& operator<< (const ::OI::Rpc::Serialize& a);

	BytesStream& operator>> (std::string& a);
	BytesStream& operator>> (Bytes& a);

	BytesStream& operator>> (::OI::Rpc::Serialize& a);

	template<typename T>
	BytesStream& operator>> (std::vector<T>& t)
	{
		uint32_t size;
		(*this) >> size;
		t.resize(size);
		for (uint32_t i = 0; i < size; ++i)
		{
			(*this) >> t[i];
		}
		return *this;
	}
	template<typename T>
	BytesStream& operator << (const std::vector<T>& t)
	{
		uint32_t size = t.size();
		(*this) << size;
		for (typename std::vector<T>::const_iterator it = t.begin(); it != t.end(); ++it)
		{
			(*this) << *it;
		}
		return *this;
	}

	// 整包：协议id + totalsize + (seqid + size + data) 重复。
	// totalsize 是后面所有 seqid+size+data 的总字节数。
	// seqid 从 0 起，按字段声明顺序对应；允许缺号、乱序。
	// 没出现的 seqid 保持字段原值，不认识的 seqid 按 size 跳过。
	// vector 的每个元素单独一条，seqid 相同。均为 uint32 小端。
	BytesStream& WritePbRaw(uint32_t seq, const void* data, uint32_t size);
	PbHeaderState PreparePbHeader(uint32_t& seq, uint32_t& size);
	void ReadPbHeader(uint32_t& seq, uint32_t& size);
	void SkipPb(uint32_t size);

	template<typename T>
	typename std::enable_if<PbFixedTrait<T>::value, BytesStream&>::type
	WritePb(uint32_t seq, const T& value)
	{
		return WritePbRaw(seq, &value, (uint32_t)sizeof(T));
	}
	BytesStream& WritePb(uint32_t seq, const std::string& value);
	BytesStream& WritePb(uint32_t seq, const Bytes& value);
	BytesStream& WritePb(uint32_t seq, const ::OI::Rpc::Serialize& value);
	template<typename T>
	BytesStream& WritePb(uint32_t seq, const std::vector<T>& value)
	{
		for (typename std::vector<T>::const_iterator it = value.begin(); it != value.end(); ++it)
		{
			WritePb(seq, *it);
		}
		return *this;
	}

	template<typename T>
	typename std::enable_if<PbFixedTrait<T>::value, void>::type
	ReadPb(uint32_t size, T& value)
	{
		if (size != (uint32_t)sizeof(T) || RemainSize() < size)
		{
			throw ParserException();
		}
		memcpy(&value, GetCurPtr(), sizeof(T));
		_cur_pos += size;
	}
	void ReadPb(uint32_t size, std::string& value);
	void ReadPb(uint32_t size, Bytes& value);
	void ReadPb(uint32_t size, ::OI::Rpc::Serialize& value);
	template<typename T>
	void ReadPb(uint32_t size, std::vector<T>& value)
	{
		T elem;
		ReadPb(size, elem);
		value.push_back(elem);
	}
	template<typename K, typename V>
	BytesStream& WritePb(uint32_t seq, const std::map<K, V>& value)
	{
		BytesStream all;
		for (typename std::map<K, V>::const_iterator it = value.begin(); it != value.end(); ++it)
		{
			BytesStream pair;
			pair.WritePb(0, it->first);
			pair.WritePb(1, it->second);
			uint32_t pair_size = (uint32_t)pair.Size();
			all.WritePbRaw(0, pair_size ? pair.Begin() : NULL, pair_size);
		}
		uint32_t size = (uint32_t)all.Size();
		return WritePbRaw(seq, size ? all.Begin() : NULL, size);
	}
	template<typename K, typename V>
	void ReadPb(uint32_t size, std::map<K, V>& value)
	{
		if (RemainSize() < size)
		{
			throw ParserException();
		}
		BytesStream all;
		if (size > 0)
		{
			all.Append(GetCurPtr(), size);
		}
		_cur_pos += size;
		value.clear();
		while (all.RemainSize() > 0)
		{
			uint32_t pair_seq = 0;
			uint32_t pair_size = 0;
			all.ReadPbHeader(pair_seq, pair_size);
			BytesStream pair;
			if (pair_size > 0)
			{
				pair.Append(all.GetCurPtr(), pair_size);
			}
			all.AddCurPos(pair_size);
			K key;
			V val;
			bool got_key = false;
			bool got_val = false;
			while (pair.RemainSize() > 0)
			{
				uint32_t field_seq = 0;
				uint32_t field_size = 0;
				pair.ReadPbHeader(field_seq, field_size);
				if (field_seq == 0)
				{
					pair.ReadPb(field_size, key);
					got_key = true;
				}
				else if (field_seq == 1)
				{
					pair.ReadPb(field_size, val);
					got_val = true;
				}
				else
				{
					pair.SkipPb(field_size);
				}
			}
			if (!got_key || !got_val)
			{
				throw ParserException();
			}
			value.insert(std::make_pair(key, val));
		}
	}

	/*
	template<typename Key, typename Value>
	BytesStream& operator>> (std::map<Key, Value>& t)
	{

	}

	template<typename Key, typename Value>
	BytesStream& operator<< (std::map<Key, Value>& t)
	{

	}	
	*/




};
}}
